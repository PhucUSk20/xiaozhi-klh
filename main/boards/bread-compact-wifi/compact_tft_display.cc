#include "compact_tft_display.h"
#include "compact_caro_board.h"
#include "compact_tft_emotion.h"

#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
#include "application.h"
#include "board.h"
#include "compact_tft_image_info.h"
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
#include "compact_tft_gif_raster.h"
#endif
#include "display/lvgl_display/jpg/jpeg_to_image.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <strings.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#if CONFIG_BREAD_COMPACT_WIFI_TFT_WORD_IMAGES_SD
#include <cJSON.h>
#include <psa/crypto.h>
#include <sys/stat.h>
#include <vector>
#include "compact_sd_word_manifest.h"
#endif

LV_FONT_DECLARE(font_noto_sans_basic_30_4);

namespace {
constexpr char kTag[] = "CompactTft";
constexpr size_t kMaxImageBytes = 128 * 1024;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
constexpr unsigned kMaxImageDimension = 480;
#else
constexpr unsigned kMaxImageDimension = 320;
#endif
constexpr uint32_t kMaxImagePixels = kMaxImageDimension * kMaxImageDimension;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
constexpr const char* kIdleEmojis[] = {
    "neutral.gif",     "happy.gif",   "laughing.gif", "funny.gif",     "loving.gif",
    "kissy.gif",       "sad.gif",     "crying.gif",   "angry.gif",     "sleepy.gif",
    "surprised.gif",   "shocked.gif", "thinking.gif", "confident.gif", "sideeye.gif",
    "embarrassed.gif", "winking.gif", "silly.gif",    "delicious.gif", "cool.gif",
    "relaxed.gif"};
#endif

bool IsValidImage(const lv_img_dsc_t* image) {
    const auto& header = image->header;
    if (header.w == 0 || header.h == 0 ||
        static_cast<uint32_t>(header.w) * header.h > kMaxImagePixels) {
        return false;
    }
    return true;
}

class OwnedPngImage : public LvglImage {
public:
    OwnedPngImage(void* data, size_t size) : data_(data) {
        CompactTftImageInfo info;
        if (!ParseCompactTftImage(static_cast<const uint8_t*>(data), size, info,
                                  kMaxImageDimension) ||
            info.jpeg) {
            return;
        }
        descriptor_.data = static_cast<const uint8_t*>(data);
        descriptor_.data_size = size;
        if (lv_image_decoder_get_info(&descriptor_, &descriptor_.header) != LV_RESULT_OK) {
            return;
        }
        valid_ = IsValidImage(&descriptor_);
    }
    ~OwnedPngImage() override { heap_caps_free(data_); }
    bool IsValid() const { return valid_; }
    const lv_img_dsc_t* image_dsc() const override { return &descriptor_; }

private:
    lv_img_dsc_t descriptor_{};
    bool valid_ = false;
    void* data_;
};

struct HeapFree {
    void operator()(void* ptr) const { heap_caps_free(ptr); }
};

std::unique_ptr<LvglImage> DecodeJpeg(const uint8_t* data, size_t size) {
    CompactTftImageInfo info;
    if (!ParseCompactTftImage(data, size, info, kMaxImageDimension) || !info.jpeg) {
        return nullptr;
    }
    uint8_t* pixels = nullptr;
    size_t bytes = 0, width = 0, height = 0, stride = 0;
    if (jpeg_to_image(data, size, &pixels, &bytes, &width, &height, &stride) != ESP_OK) {
        return nullptr;
    }
    std::unique_ptr<void, HeapFree> owner(pixels);
    std::unique_ptr<LvglImage> image(new (std::nothrow) LvglAllocatedImage(
        pixels, bytes, width, height, stride, LV_COLOR_FORMAT_RGB565));
    if (!image) {
        return nullptr;
    }
    owner.release();
    return image;
}
}  // namespace

CompactTftDisplay::CompactTftDisplay(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_handle_t panel, int width, int height,
                                     bool mirror_x, bool mirror_y, bool swap_xy,
                                     bool initialize_lvgl)
    : panel_io_(panel_io), panel_(panel), owns_lvgl_port_(initialize_lvgl) {
    width_ = width;
    height_ = height;
    // Normally the primary OLED owns LVGL initialization. Do not initialize it twice.
    if (owns_lvgl_port_) {
        lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
        ESP_ERROR_CHECK(lvgl_port_init(&cfg));
    }
    DisplayLockGuard lock(this);
    auto* primary_display = lv_display_get_default();
    lvgl_port_display_cfg_t cfg = {};
    cfg.io_handle = panel_io_;
    cfg.panel_handle = panel_;
    cfg.buffer_size = width_ * 20;
    cfg.hres = width_;
    cfg.vres = height_;
    cfg.rotation.swap_xy = swap_xy;
    cfg.rotation.mirror_x = mirror_x;
    cfg.rotation.mirror_y = mirror_y;
    cfg.color_format = LV_COLOR_FORMAT_RGB565;
    cfg.flags.buff_dma = true;
    cfg.flags.swap_bytes = true;
    display_ = lvgl_port_add_disp(&cfg);
    if (primary_display != nullptr) {
        // OLED UI code uses lv_screen_active(), so keep its display as the default.
        lv_display_set_default(primary_display);
    }
    if (display_ == nullptr) {
        ESP_LOGE(kTag, "Could not register secondary TFT display");
    }
}

bool CompactTftDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void CompactTftDisplay::Unlock() { lvgl_port_unlock(); }

CompactTftDisplay::~CompactTftDisplay() {
    {
        DisplayLockGuard lock(this);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
        if (emotion_timer_ != nullptr) {
            lv_timer_delete(emotion_timer_);
            emotion_timer_ = nullptr;
        }
        StopSdGifLocked();
#endif
        if (image_layer_ != nullptr) {
            lv_obj_delete(image_layer_);
        }
        if (fullscreen_image_data_) {
            lv_image_cache_drop(fullscreen_image_data_->image_dsc());
            fullscreen_image_data_.reset();
        }
    }
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    sd_card_.reset();
#endif
    if (display_ != nullptr) {
        lvgl_port_remove_disp(display_);
    }
    esp_lcd_panel_del(panel_);
    esp_lcd_panel_io_del(panel_io_);
    if (owns_lvgl_port_) {
        lvgl_port_deinit();
    }
}

void CompactTftDisplay::SetupUI() {
    if (IsSetupUICalled() || display_ == nullptr) {
        return;
    }
    Display::SetupUI();
    {
        DisplayLockGuard lock(this);
        auto* screen = lv_display_get_screen_active(display_);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        image_layer_ = lv_obj_create(screen);
        lv_obj_remove_style_all(image_layer_);
        lv_obj_set_size(image_layer_, width_, height_);
        lv_obj_set_style_bg_color(image_layer_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(image_layer_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(image_layer_, LV_OBJ_FLAG_SCROLLABLE);
        fullscreen_image_ = lv_image_create(image_layer_);
        vocabulary_label_ = lv_label_create(image_layer_);
        lv_obj_set_width(vocabulary_label_, width_ - 12);
        lv_label_set_long_mode(vocabulary_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_color(vocabulary_label_, lv_color_white(), 0);
        lv_obj_set_style_text_align(vocabulary_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(vocabulary_label_);
        lv_obj_add_flag(vocabulary_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(image_layer_, LV_OBJ_FLAG_HIDDEN);
        error_label_ = lv_label_create(screen);
        lv_obj_set_width(error_label_, width_ - 8);
        lv_obj_set_style_text_color(error_label_, lv_color_white(), 0);
        lv_obj_set_style_text_align(error_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(error_label_);
        lv_obj_add_flag(error_label_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(kTag, "Secondary TFT ready (%dx%d); waiting for image tool", width_, height_);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    sd_card_ = std::make_unique<CompactSdGif>();
    sd_card_->Start();
    {
        DisplayLockGuard lock(this);
        emotion_timer_ = lv_timer_create(EmotionTimer, 100, this);
    }
#endif
}

void CompactTftDisplay::ShowNotification(const char* message, int /*duration_ms*/) {
    DisplayLockGuard lock(this);
    if (error_label_ == nullptr || game_mode_) {
        return;
    }
    lv_label_set_text(error_label_, message);
    lv_obj_center(error_label_);
    lv_obj_remove_flag(error_label_, LV_OBJ_FLAG_HIDDEN);
}

void CompactTftDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (fullscreen_image_ == nullptr || game_mode_) {
        return;
    }
    text_visible_ = false;
    caro_visible_ = false;
    if (caro_panel_ != nullptr)
        lv_obj_add_flag(caro_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(vocabulary_label_, LV_OBJ_FLAG_HIDDEN);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    StopSdGifLocked();
#endif
    lv_obj_add_flag(error_label_, LV_OBJ_FLAG_HIDDEN);
    if (!image) {
        lv_obj_add_flag(image_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(fullscreen_image_, nullptr);
        if (fullscreen_image_data_) {
            lv_image_cache_drop(fullscreen_image_data_->image_dsc());
        }
        fullscreen_image_data_.reset();
        return;
    }
    const auto* descriptor = image->image_dsc();
    if (!IsValidImage(descriptor)) {
        ESP_LOGE(kTag, "Refusing image with invalid dimensions");
        return;
    }
    // Update LVGL's source before freeing the previous image buffer.
    lv_image_set_src(fullscreen_image_, descriptor);
    if (fullscreen_image_data_) {
        lv_image_cache_drop(fullscreen_image_data_->image_dsc());
    }
    fullscreen_image_data_ = std::move(image);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    submitted_emotion_gif_.clear();
#endif
    const uint32_t scale =
        std::min(256u * width_ / descriptor->header.w, 256u * height_ / descriptor->header.h);
    lv_image_set_scale(fullscreen_image_, scale);
    lv_obj_center(fullscreen_image_);
    lv_obj_remove_flag(image_layer_, LV_OBJ_FLAG_HIDDEN);
}

bool CompactTftDisplay::ShowText(const std::string& text) {
    if (text.empty() || text.size() > 80 ||
        text.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") ==
            std::string::npos ||
        text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz '-") !=
            std::string::npos) {
        throw std::runtime_error("Use an English word or phrase of 1..80 ASCII characters");
    }
    DisplayLockGuard lock(this);
    if (game_mode_)
        throw std::runtime_error("Rock Paper Scissors owns the TFT");
    if (vocabulary_label_ == nullptr)
        return false;
    ++visual_generation_;  // Supersede an in-flight GIF/image, without waiting for network I/O.
    SetPreviewImage(nullptr);
    text_visible_ = true;
    lv_label_set_text(vocabulary_label_, text.c_str());
    lv_obj_center(vocabulary_label_);
    lv_obj_remove_flag(vocabulary_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(image_layer_, LV_OBJ_FLAG_HIDDEN);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    submitted_emotion_gif_.clear();
#endif
    ESP_LOGI(kTag, "Vocabulary text displayed (%u bytes)", static_cast<unsigned>(text.size()));
    return true;
}

void CompactTftDisplay::ShowGameScreen(const char* title, const char* value, const char* hint) {
    DisplayLockGuard lock(this);
    if (image_layer_ == nullptr) {
        ESP_LOGW(kTag, "Cannot display game: TFT UI is not ready");
        return;
    }
    if (!game_mode_) {
        ++visual_generation_;      // Cancel pending network/SD display workers.
        SetPreviewImage(nullptr);  // Stop SD animation and hide any Caro/text/image.
        game_mode_ = true;
    }
    if (game_panel_ == nullptr) {
        game_panel_ = lv_obj_create(image_layer_);
        lv_obj_remove_style_all(game_panel_);
        lv_obj_set_size(game_panel_, width_, height_);
        lv_obj_remove_flag(game_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(game_panel_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(game_panel_, LV_OPA_COVER, 0);
        game_title_ = lv_label_create(game_panel_);
        game_value_ = lv_label_create(game_panel_);
        game_hint_ = lv_label_create(game_panel_);
        for (auto* label : {game_title_, game_value_, game_hint_}) {
            lv_obj_set_width(label, width_ - 8);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(label, lv_color_white(), 0);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        }
        lv_obj_align(game_title_, LV_ALIGN_TOP_MID, 0, height_ / 12);
        lv_obj_set_style_text_font(game_value_, &font_noto_sans_basic_30_4, 0);
        lv_obj_align(game_value_, LV_ALIGN_CENTER, 0, -4);
        lv_obj_align(game_hint_, LV_ALIGN_BOTTOM_MID, 0, -height_ / 12);
    }
    lv_label_set_text(game_title_, title);
    lv_label_set_text(game_value_, value);
    lv_label_set_text(game_hint_, hint);
    lv_obj_remove_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(game_panel_, -1);
    lv_obj_remove_flag(image_layer_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(kTag, "Game screen: %s %s", title, value);
}

void CompactTftDisplay::ShowGameCountdown(int digit) {
    char value[2] = {static_cast<char>('0' + std::clamp(digit, 1, 3)), 0};
    ShowGameScreen("READY", value, "Rock Paper Scissors");
}

void CompactTftDisplay::ShowGameRound(int round, int total_rounds) {
    char value[32];
    total_rounds = std::max(total_rounds, 1);
    std::snprintf(value, sizeof(value), "%d / %d", std::clamp(round, 1, total_rounds),
                  total_rounds);
    ShowGameScreen("ROUND", value, "Hand below 10 cm");
}

void CompactTftDisplay::EndGameMode() {
    DisplayLockGuard lock(this);
    if (!game_mode_)
        return;
    game_mode_ = false;
    lv_obj_add_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    ShowEmoji();
    ESP_LOGI(kTag, "Game screen closed");
}

void CompactTftDisplay::BuildCaroPanelLocked() {
    const int kCell = std::min(width_, height_) / 3;
    const int inset = kCell / 5;
    const auto start = static_cast<lv_value_precise_t>(inset);
    const auto end = static_cast<lv_value_precise_t>(kCell - inset);
    caro_x1_points_[0] = {start, start};
    caro_x1_points_[1] = {end, end};
    caro_x2_points_[0] = {end, start};
    caro_x2_points_[1] = {start, end};
    const int x0 = (width_ - 3 * kCell) / 2;
    const int y0 = (height_ - 3 * kCell) / 2;
    caro_panel_ = lv_obj_create(image_layer_);
    lv_obj_remove_style_all(caro_panel_);
    lv_obj_set_size(caro_panel_, width_, height_);
    lv_obj_set_pos(caro_panel_, 0, 0);
    lv_obj_remove_flag(caro_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(caro_panel_, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 9; ++i) {
        lv_obj_t* box = lv_obj_create(caro_panel_);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, kCell, kCell);
        lv_obj_set_pos(box, x0 + (i % 3) * kCell, y0 + (i / 3) * kCell);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, lv_color_make(90, 90, 90), 0);
        lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
        CaroCell& c = caro_cells_[i];
        c.num = lv_label_create(box);
        lv_obj_set_style_text_font(c.num, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(c.num, lv_color_make(110, 110, 110), 0);
        lv_obj_center(c.num);
        c.circle = lv_obj_create(box);
        lv_obj_remove_style_all(c.circle);
        lv_obj_set_size(c.circle, kCell - 2 * inset, kCell - 2 * inset);
        lv_obj_center(c.circle);
        lv_obj_set_style_radius(c.circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(c.circle, 4, 0);
        lv_obj_set_style_border_color(c.circle, lv_color_make(80, 200, 255), 0);
        lv_obj_set_style_border_opa(c.circle, LV_OPA_COVER, 0);
        lv_obj_add_flag(c.circle, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t** line : {&c.x1, &c.x2}) {
            *line = lv_line_create(box);
            lv_line_set_points(*line, line == &c.x1 ? caro_x1_points_ : caro_x2_points_, 2);
            lv_obj_set_pos(*line, 0, 0);
            lv_obj_set_style_line_width(*line, 4, 0);
            lv_obj_set_style_line_color(*line, lv_color_make(255, 200, 60), 0);
            lv_obj_set_style_line_rounded(*line, true, 0);
            lv_obj_add_flag(*line, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void CompactTftDisplay::ShowCaroBoard(const std::string& cells) {
    if (!IsValidCompactCaroBoard(cells)) {
        throw std::runtime_error("cells must contain 9 squares: their own digit, X or O");
    }
    DisplayLockGuard lock(this);
    if (game_mode_)
        throw std::runtime_error("Rock Paper Scissors owns the TFT");
    if (image_layer_ == nullptr) {
        throw std::runtime_error("Caro TFT is not ready");
    }
    ++visual_generation_;  // Every board update supersedes older image/GIF workers.
    if (!caro_visible_) {
        SetPreviewImage(nullptr);
    }
    if (caro_panel_ == nullptr) {
        BuildCaroPanelLocked();
    }
    for (int i = 0; i < 9; ++i) {
        const char ch = cells[i];
        CaroCell& c = caro_cells_[i];
        const bool is_x = ch == 'X';
        const bool is_o = ch == 'O';
        if (is_x) {
            lv_obj_remove_flag(c.x1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(c.x2, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(c.x1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(c.x2, LV_OBJ_FLAG_HIDDEN);
        }
        if (is_o) {
            lv_obj_remove_flag(c.circle, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(c.circle, LV_OBJ_FLAG_HIDDEN);
        }
        if (is_x || is_o) {
            lv_obj_add_flag(c.num, LV_OBJ_FLAG_HIDDEN);
        } else {
            char digit[2] = {ch, 0};
            lv_label_set_text(c.num, digit);
            lv_obj_remove_flag(c.num, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_flag(caro_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(image_layer_, LV_OBJ_FLAG_HIDDEN);
    caro_visible_ = true;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    submitted_emotion_gif_.clear();
#endif
    ESP_LOGI(kTag, "Caro board displayed: %s", cells.c_str());
}

void CompactTftDisplay::EndCaroBoard() {
    DisplayLockGuard lock(this);
    if (!caro_visible_) {
        return;
    }
    ShowEmoji();  // hides the board, resumes the emotion GIFs
    ESP_LOGI(kTag, "Caro board closed");
}

void CompactTftDisplay::ShowEmoji() {
    DisplayLockGuard lock(this);
    if (game_mode_)
        return;
    ++visual_generation_;
    SetPreviewImage(nullptr);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    submitted_emotion_gif_.clear();  // Resume even if the emotion hasn't changed.
#endif
}

struct CompactTftDisplay::DownloadJob {
    CompactTftDisplay* display;
    std::string url;
    uint32_t generation;
    std::string word_key{};
};

#if CONFIG_BREAD_COMPACT_WIFI_TFT_WORD_IMAGES_SD
namespace {
struct SdFileBytes {
    std::unique_ptr<void, HeapFree> data;
    size_t size = 0;
};

bool ReadBoundedSdFile(const std::string& path, size_t limit, SdFileBytes& bytes, char* failure,
                       size_t failure_size) {
    struct stat info{};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<size_t>(info.st_size) > limit) {
        std::snprintf(failure, failure_size, "SD file missing/empty/oversized: %s", path.c_str());
        return false;
    }
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!file) {
        std::snprintf(failure, failure_size, "Could not open SD file: %s", path.c_str());
        return false;
    }
    const size_t size = static_cast<size_t>(info.st_size);
    // Large manifests should use PSRAM when available. Both allocation paths are non-throwing.
    bytes.data.reset(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bytes.data)
        bytes.data.reset(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    if (!bytes.data) {
        std::snprintf(failure, failure_size, "Not enough memory for SD file (%u bytes)",
                      static_cast<unsigned>(size));
        return false;
    }
    if (std::fread(bytes.data.get(), 1, size, file.get()) != size) {
        std::snprintf(failure, failure_size, "Incomplete SD read: %s", path.c_str());
        return false;
    }
    bytes.size = size;
    return true;
}
}  // namespace

bool CompactTftDisplay::LoadSdWordImageIndex(char* failure, size_t failure_size) {
    if (!sd_word_images_.empty())
        return true;
    SdFileBytes bytes;
    if (!ReadBoundedSdFile("/sdcard/manifest.json", kCompactSdManifestMaxBytes, bytes, failure,
                           failure_size))
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLength(static_cast<const char*>(bytes.data.get()), bytes.size),
        cJSON_Delete);
    bytes.data.reset();  // cJSON owns copied strings; release raw text before building the index.
    if (!root) {
        std::snprintf(failure, failure_size,
                      "Invalid SD manifest JSON or insufficient parse memory");
        return false;
    }
    const auto* version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    auto* images = cJSON_GetObjectItemCaseSensitive(root.get(), "images");
    const auto* count = cJSON_GetObjectItemCaseSensitive(root.get(), "count");
    const int actual_count = cJSON_GetArraySize(images);
    if (!cJSON_IsNumber(version) || !cJSON_IsArray(images) || !cJSON_IsNumber(count) ||
        !IsCompactSdManifestHeader(version->valuedouble, count->valuedouble, actual_count)) {
        ESP_LOGE(kTag, "Unsupported SD manifest: version=%.0f count=%.0f images=%d limit=%d",
                 cJSON_IsNumber(version) ? version->valuedouble : -1.0,
                 cJSON_IsNumber(count) ? count->valuedouble : -1.0, actual_count,
                 kCompactSdManifestMaxEntries);
        std::snprintf(failure, failure_size, "Unsupported SD manifest (v1, 1..%d matching entries)",
                      kCompactSdManifestMaxEntries);
        return false;
    }
    std::map<std::string, SdWordImage> index;
    int position = 0;
    while (images->child != nullptr) {
        const auto* item = images->child;
        const auto* key = cJSON_GetObjectItemCaseSensitive(item, "key");
        const auto* lesson = cJSON_GetObjectItemCaseSensitive(item, "lessonId");
        const auto* file = cJSON_GetObjectItemCaseSensitive(item, "file");
        const auto* sha = cJSON_GetObjectItemCaseSensitive(item, "sha256");
        if (!cJSON_IsString(key) || !cJSON_IsString(lesson) || !cJSON_IsString(file) ||
            !cJSON_IsString(sha) ||
            !IsCompactSdWordMapping(key->valuestring, lesson->valuestring, file->valuestring,
                                    sha->valuestring)) {
            std::snprintf(failure, failure_size, "Invalid SD manifest entry %d", position + 1);
            return false;
        }
        if (!index.emplace(key->valuestring, SdWordImage{file->valuestring, sha->valuestring})
                 .second) {
            std::snprintf(failure, failure_size, "Duplicate SD manifest key at entry %d",
                          position + 1);
            return false;
        }
        // Drop optional captions/credits as each mapping is copied, reducing peak parse/index RAM.
        cJSON_DeleteItemFromArray(images, 0);
        ++position;
    }
    sd_word_images_ = std::move(index);  // Publish only a complete, validated index.
    ESP_LOGI(kTag, "SD word image index loaded: %u entries (limit %d)",
             static_cast<unsigned>(sd_word_images_.size()), kCompactSdManifestMaxEntries);
    return true;
}

std::unique_ptr<LvglImage> CompactTftDisplay::LoadSdWordImage(const std::string& key, char* failure,
                                                              size_t failure_size) {
    if (!sd_card_ || !sd_card_->IsReady()) {
        std::snprintf(failure, failure_size, "SD card is not ready");
        return nullptr;
    }
    if (!LoadSdWordImageIndex(failure, failure_size))
        return nullptr;
    const auto entry = sd_word_images_.find(key);
    if (entry == sd_word_images_.end()) {
        std::snprintf(failure, failure_size, "Word image not found in SD manifest");
        return nullptr;
    }
    SdFileBytes bytes;
    if (!ReadBoundedSdFile("/sdcard/" + entry->second.file, kMaxImageBytes, bytes, failure,
                           failure_size))
        return nullptr;
    unsigned char digest[32];
    size_t digest_size = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, static_cast<const uint8_t*>(bytes.data.get()), bytes.size,
                         digest, sizeof(digest), &digest_size) != PSA_SUCCESS ||
        digest_size != sizeof(digest)) {
        std::snprintf(failure, failure_size, "Could not verify SD image");
        return nullptr;
    }
    char hex[65]{};
    for (size_t i = 0; i < sizeof(digest); ++i)
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (entry->second.sha256 != hex) {
        std::snprintf(failure, failure_size, "SD image checksum mismatch");
        return nullptr;
    }
    CompactTftImageInfo info;
    const auto* data = static_cast<const uint8_t*>(bytes.data.get());
    if (!ParseCompactTftImage(data, bytes.size, info, kMaxImageDimension) || !info.jpeg) {
        std::snprintf(failure, failure_size, "Invalid SD JPEG image");
        return nullptr;
    }
    auto image = DecodeJpeg(data, bytes.size);
    if (!image)
        std::snprintf(failure, failure_size, "SD JPEG decoding failed");
    return image;
}

bool CompactTftDisplay::ShowWordImage(const std::string& lesson_id, const std::string& key) {
    if (lesson_id.empty() || lesson_id.size() > 100 || key.size() > 260 ||
        key.size() <= lesson_id.size() + 1 || key.rfind(lesson_id + ":", 0) != 0)
        throw std::runtime_error("Use exact lessonId and full key from nextWord/saveTarget");
    DisplayLockGuard lock(this);
    if (game_mode_)
        return false;
    auto job = std::make_unique<DownloadJob>(DownloadJob{this, "", 0, key});
    bool expected = false;
    if (!downloading_.compare_exchange_strong(expected, true))
        return false;
    job->generation = ++visual_generation_;
    image_generation_ = job->generation;
    if (xTaskCreate(DownloadTask, "tft_sd_word", 8192, job.get(), 2, nullptr) != pdPASS) {
        downloading_ = false;
        throw std::runtime_error("Could not start SD image task");
    }
    job.release();
    return true;
}
#endif

#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
void CompactTftDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    requested_emotion_ = emotion && std::strlen(emotion) <= 32 ? emotion : "neutral";
    if (strcasecmp(requested_emotion_.c_str(), "neutral") == 0 ||
        strcasecmp(requested_emotion_.c_str(), "normal") == 0 ||
        strcasecmp(requested_emotion_.c_str(), "idle") == 0)
        requested_emotion_ = "neutral";
    UpdateEmotionLocked();
}

void CompactTftDisplay::SetRuntimeState(DeviceState state) {
    DisplayLockGuard lock(this);
    if (state == kDeviceStateIdle || state == kDeviceStateListening ||
        state == kDeviceStateConnecting)
        requested_emotion_ = "neutral";
    UpdateEmotionLocked();
}

void CompactTftDisplay::UpdateEmotionLocked() {
    const auto state = Application::GetInstance().GetDeviceState();
    const auto resolved = ResolveCompactTftEmotion(state, requested_emotion_);
    const char* emotion = resolved.data();
    // Keep only the newest request; the timer waits for mounting or a busy image worker.
    const char* name = "neutral.gif";
    struct Mapping {
        const char* emotion;
        const char* gif;
    };
    static constexpr Mapping mappings[] = {{"neutral", "neutral.gif"},
                                           {"idle", "neutral.gif"},
                                           {"happy", "happy.gif"},
                                           {"laughing", "laughing.gif"},
                                           {"funny", "funny.gif"},
                                           {"chatty", "happy.gif"},
                                           {"glee", "happy.gif"},
                                           {"excited", "happy.gif"},
                                           {"loving", "loving.gif"},
                                           {"love", "loving.gif"},
                                           {"kissy", "kissy.gif"},
                                           {"sad", "sad.gif"},
                                           {"crying", "crying.gif"},
                                           {"angry", "angry.gif"},
                                           {"sleepy", "sleepy.gif"},
                                           {"bored", "relaxed.gif"},
                                           {"surprised", "surprised.gif"},
                                           {"shocked", "shocked.gif"},
                                           {"nervous", "shocked.gif"},
                                           {"thinking", "thinking.gif"},
                                           {"focused", "thinking.gif"},
                                           {"confident", "confident.gif"},
                                           {"curious", "thinking.gif"},
                                           {"attentive", "thinking.gif"},
                                           {"confused", "sideeye.gif"},
                                           {"skeptic", "sideeye.gif"},
                                           {"sideeye", "sideeye.gif"},
                                           {"eyeroll", "eyeroll.gif"},
                                           {"deadpan", "deadpan.gif"},
                                           {"hit", "hit.gif"},
                                           {"starstruck", "starstruck.gif"},
                                           {"embarrassed", "embarrassed.gif"},
                                           {"winking", "winking.gif"},
                                           {"wink", "winking.gif"},
                                           {"silly", "silly.gif"},
                                           {"mischievous", "silly.gif"},
                                           {"delicious", "delicious.gif"},
                                           {"cool", "cool.gif"},
                                           {"relaxed", "relaxed.gif"}};
    if (emotion != nullptr) {
        for (const auto& mapping : mappings) {
            if (strcasecmp(emotion, mapping.emotion) == 0) {
                name = mapping.gif;
                break;
            }
        }
    }
    const std::string desired = state == kDeviceStateIdle ? kIdleEmojis[idle_emoji_index_] : name;
    if (desired_emotion_gif_ != desired) {
        ESP_LOGI(kTag, "Emotion requested=%s visible=%s GIF=%s", requested_emotion_.c_str(),
                 emotion, desired.c_str());
        desired_emotion_gif_ = desired;
    }
}

void CompactTftDisplay::CycleIdleEmoji() {
    DisplayLockGuard lock(this);
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
        return;
    idle_emoji_index_ = (idle_emoji_index_ + 1) % (sizeof(kIdleEmojis) / sizeof(kIdleEmojis[0]));
    ShowEmoji();
    desired_emotion_gif_ = kIdleEmojis[idle_emoji_index_];
    ESP_LOGI(kTag, "Idle emoji %u/21: %s", static_cast<unsigned>(idle_emoji_index_ + 1),
             desired_emotion_gif_.c_str());
}

void CompactTftDisplay::EmotionTimer(lv_timer_t* timer) {
    auto* display = static_cast<CompactTftDisplay*>(lv_timer_get_user_data(timer));
    if (display->game_mode_)
        return;
    const bool idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    if (idle && !display->was_idle_) {
        display->idle_emoji_index_ = 0;
        display->ShowEmoji();
        display->desired_emotion_gif_ = kIdleEmojis[0];
    }
    display->was_idle_ = idle;
    if ((display->text_visible_ || display->fullscreen_image_data_ || display->caro_visible_) &&
        Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        display->ShowEmoji();
    }
    // Keep vocabulary through speaking/listening; explicit exit or idle releases it.
    // A canceled network worker must not delay restoring the local SD animation.
    if (!display->text_visible_ && !display->fullscreen_image_data_ && !display->caro_visible_ &&
        display->desired_emotion_gif_ != display->submitted_emotion_gif_ &&
        (!display->IsDownloading() ||
         display->image_generation_.load() != display->visual_generation_.load()) &&
        display->PlaySdGif(display->desired_emotion_gif_)) {
        display->submitted_emotion_gif_ = display->desired_emotion_gif_;
    }
}

void CompactTftDisplay::StopSdGifLocked() {
    if (gif_frame_timer_ != nullptr) {
        lv_timer_delete(gif_frame_timer_);
        gif_frame_timer_ = nullptr;
    }
    if (gif_widget_ != nullptr) {
        lv_obj_delete(gif_widget_);
        gif_widget_ = nullptr;
        gif_path_.clear();
    }
    lv_image_cache_drop(&gif_descriptor_);
    if (gif_decoder_ != nullptr) {
        gd_close_gif(gif_decoder_);
        gif_decoder_ = nullptr;
    }
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
    heap_caps_free(gif_raster_);
    gif_raster_ = nullptr;
#endif
    gif_descriptor_ = {};
}

bool CompactTftDisplay::RenderSdGifFrame() {
    gd_render_frame(gif_decoder_, gif_decoder_->canvas);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
    return CompactTftGifRaster(
        gif_decoder_->canvas, size_t(gif_decoder_->width) * gif_decoder_->height * 4,
        gif_decoder_->width, gif_decoder_->height, gif_raster_, gif_descriptor_.data_size,
        gif_descriptor_.header.w, gif_descriptor_.header.h);
#else
    return true;
#endif
}

void CompactTftDisplay::GifFrameTimer(lv_timer_t* timer) {
    auto* display = static_cast<CompactTftDisplay*>(lv_timer_get_user_data(timer));
    auto* gif = display->gif_decoder_;
    if (gif == nullptr || display->gif_widget_ == nullptr)
        return;
    const int frame = gd_get_frame(gif);
    if (frame <= 0) {
        ESP_LOGW(kTag, "GIF frame decode failed: %s", display->gif_path_.c_str());
        lv_timer_pause(timer);
        return;
    }
    // Disposal=1 retains the composed canvas; transparent delta pixels do not erase it.
    gif->loop_count = 0;  // Infinite, regardless of loop metadata in the file.
    if (!display->RenderSdGifFrame()) {
        ESP_LOGE(kTag, "GIF raster dimensions/buffer invalid: %s", display->gif_path_.c_str());
        lv_timer_pause(timer);
        return;
    }
    lv_image_cache_drop(&display->gif_descriptor_);
    lv_obj_invalidate(display->gif_widget_);
    lv_timer_set_period(timer, std::max(20u, static_cast<unsigned>(gif->gce.delay) * 10u));
}

bool CompactTftDisplay::PlaySdGif(const std::string& name) {
    DisplayLockGuard lock(this);
    if (game_mode_)
        return false;
    if (downloading_ && image_generation_.load() == visual_generation_.load()) {
        return false;
    }
    if (!sd_card_ || !sd_card_->Contains(name)) {
        return false;
    }
    auto job = std::make_unique<DownloadJob>(DownloadJob{this, name, visual_generation_.load()});
    bool expected = false;
    if (!gif_loading_.compare_exchange_strong(expected, true)) {
        return false;
    }
    if (xTaskCreate(SdGifTask, "sd_gif_play", 6144, job.get(), 2, nullptr) != pdPASS) {
        gif_loading_ = false;
        return false;
    }
    job.release();
    return true;
}

void CompactTftDisplay::SdGifTask(void* arg) {
    {
        std::unique_ptr<DownloadJob> job(static_cast<DownloadJob*>(arg));
        auto* display = job->display;
        unsigned width = 0, height = 0;
        bool loaded = false;
        if (display->sd_card_->ValidateFile(job->url, width, height)) {
            DisplayLockGuard lock(display);
            if (job->generation == display->visual_generation_.load()) {
                display->SetPreviewImage(nullptr);
                display->gif_path_ = std::string("S:/") + CompactSdGif::kRelativePrefix + job->url;
                display->gif_decoder_ = gd_open_gif_file(display->gif_path_.c_str());
                if (display->gif_decoder_ != nullptr) {
                    auto* gif = display->gif_decoder_;
                    loaded = gif->width == width && gif->height == height && gd_get_frame(gif) > 0;
                    if (loaded) {
                        gif->loop_count = 0;
                        auto& descriptor = display->gif_descriptor_;
                        descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
                        descriptor.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
                        const auto size =
                            CompactTftGifFit(width, height, display->width_, display->height_);
                        descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
                        descriptor.header.w = size.width;
                        descriptor.header.h = size.height;
                        descriptor.header.stride = size.width * 2;
                        descriptor.data_size = size.width * size.height * 2;
                        display->gif_raster_ = static_cast<uint8_t*>(heap_caps_malloc(
                            descriptor.data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                        descriptor.data = display->gif_raster_;
#else
                        descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
                        descriptor.header.w = width;
                        descriptor.header.h = height;
                        descriptor.header.stride = width * 4;
                        descriptor.data = gif->canvas;
                        descriptor.data_size = width * height * 4;
#endif
                        loaded = display->RenderSdGifFrame();
                    }
                    if (loaded) {
                        display->gif_widget_ = lv_image_create(display->image_layer_);
                        if (display->gif_widget_ == nullptr) {
                            loaded = false;
                        } else {
                            lv_image_set_src(display->gif_widget_, &display->gif_descriptor_);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
                            // Raster is already fitted. Avoid LVGL's ARGB8888
                            // transform across partial draw-buffer strips.
                            lv_image_set_scale(display->gif_widget_, 256);
                            lv_obj_set_size(display->gif_widget_, display->gif_descriptor_.header.w,
                                            display->gif_descriptor_.header.h);
                            ESP_LOGI(kTag, "GIF raster: %ux%u -> %ux%u, RGB565 stride=%u, LVGL 1:1",
                                     width, height, display->gif_descriptor_.header.w,
                                     display->gif_descriptor_.header.h,
                                     display->gif_descriptor_.header.stride);
#else
                            const uint32_t scale = std::min(256u * display->width_ / width,
                                                            256u * display->height_ / height);
                            lv_image_set_scale(display->gif_widget_, scale);
                            lv_obj_set_size(display->gif_widget_, width, height);
#endif
                            lv_obj_center(display->gif_widget_);
                            display->gif_frame_timer_ = lv_timer_create(
                                GifFrameTimer,
                                std::max(20u, static_cast<unsigned>(gif->gce.delay) * 10u),
                                display);
                            loaded = display->gif_frame_timer_ != nullptr;
                            if (loaded)
                                lv_obj_remove_flag(display->image_layer_, LV_OBJ_FLAG_HIDDEN);
                        }
                    }
                }
                if (!loaded)
                    display->StopSdGifLocked();
            }
        }
        if (loaded) {
            ESP_LOGI(kTag,
                     "SD GIF playing: %s (%ux%u), gifdec composed canvas, TFT %dx%d, infinite loop",
                     job->url.c_str(), width, height, display->width_, display->height_);
        } else {
            DisplayLockGuard lock(display);
            if (job->generation == display->visual_generation_.load()) {
                ESP_LOGW(kTag, "Could not read/decode SD GIF: %s", job->url.c_str());
                display->SetPreviewImage(nullptr);
                display->ShowNotification("Could not read SD GIF", 10000);
            }
        }
        display->gif_loading_ = false;
    }
    vTaskDelete(nullptr);
}
#endif

bool CompactTftDisplay::DownloadImage(const std::string& url) {
#if CONFIG_BREAD_COMPACT_WIFI_TFT_WORD_IMAGES_SD
    throw std::runtime_error("SD image mode: use self.screen.show_word_image with lessonId/key");
#endif
    if (url.size() > 2048 || (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)) {
        throw std::runtime_error("Provide an HTTP or HTTPS image URL (at most 2048 characters)");
    }
    DisplayLockGuard lock(this);
    if (game_mode_)
        return false;
    auto job = std::make_unique<DownloadJob>(DownloadJob{this, url, 0});
    bool expected = false;
    if (!downloading_.compare_exchange_strong(expected, true)) {
        return false;
    }
    const auto generation = ++visual_generation_;
    image_generation_ = generation;
    job->generation = generation;
    if (xTaskCreate(DownloadTask, "tft_image", 6144, job.get(), 2, nullptr) != pdPASS) {
        downloading_ = false;
        throw std::runtime_error("Could not start image download task");
    }
    job.release();
    return true;
}

void CompactTftDisplay::DownloadTask(void* arg) {
    // End the scope before deleting the FreeRTOS task so C++ owners are destroyed.
    {
        std::unique_ptr<DownloadJob> job(static_cast<DownloadJob*>(arg));
        char failure[96] = "Image download failed";
        try {
            // HTTP and malformed-image failures are expected results, not exceptions.
            // Keep them inside this scope so network/buffer owners are released normally.
            const bool success = [&]() -> bool {
#if CONFIG_BREAD_COMPACT_WIFI_TFT_WORD_IMAGES_SD
                if (!job->word_key.empty()) {
                    auto image =
                        job->display->LoadSdWordImage(job->word_key, failure, sizeof(failure));
                    if (!image)
                        return false;
                    DisplayLockGuard lock(job->display);
                    if (job->generation != job->display->visual_generation_.load())
                        return true;
                    job->display->SetPreviewImage(std::move(image));
                    ESP_LOGI(kTag, "SD word image displayed: %s (no HTTP)", job->word_key.c_str());
                    return true;
                }
#endif
                auto http = Board::GetInstance().GetNetwork()->CreateHttp();
                if (!http) {
                    std::snprintf(failure, sizeof(failure), "Could not create image HTTP client");
                    return false;
                }
                http->SetTimeout(10000);
                // CloudFront rejects requests without a User-Agent (HTTP 403).
                http->SetHeader("User-Agent", "Xiaozhi/1.0");
                http->SetHeader("Accept", "image/jpeg, image/png");
                if (!http->Open("GET", job->url)) {
                    std::snprintf(failure, sizeof(failure), "Image connection failed");
                    return false;
                }
                const int status = http->GetStatusCode();
                if (status != 200) {
                    std::snprintf(failure, sizeof(failure), "Image HTTP error: %d", status);
                    http->Close();
                    return false;
                }
                const size_t size = http->GetBodyLength();
                if (size == 0 || size > kMaxImageBytes) {
                    std::snprintf(failure, sizeof(failure),
                                  "Image needs Content-Length of 1..131072 bytes");
                    return false;
                }
                std::unique_ptr<void, HeapFree> data(heap_caps_malloc(size, MALLOC_CAP_8BIT));
                if (!data) {
                    std::snprintf(failure, sizeof(failure), "Not enough memory for image");
                    return false;
                }
                size_t received = 0;
                const int64_t deadline = esp_timer_get_time() + 15000000;
                while (received < size) {
                    if (esp_timer_get_time() > deadline) {
                        std::snprintf(failure, sizeof(failure), "Image download timed out");
                        return false;
                    }
                    const int count =
                        http->Read(static_cast<char*>(data.get()) + received, size - received);
                    if (count <= 0 || static_cast<size_t>(count) > size - received) {
                        std::snprintf(failure, sizeof(failure), "Incomplete image download");
                        return false;
                    }
                    received += count;
                }
                http->Close();
                std::unique_ptr<LvglImage> image;
                CompactTftImageInfo info;
                if (!ParseCompactTftImage(static_cast<const uint8_t*>(data.get()), size, info,
                                          kMaxImageDimension)) {
                    std::snprintf(failure, sizeof(failure),
                                  "Invalid PNG/JPEG or dimensions exceed panel limit");
                    return false;
                }
                if (info.jpeg) {
                    image = DecodeJpeg(static_cast<const uint8_t*>(data.get()), size);
                } else {
                    DisplayLockGuard lock(job->display);
                    // The encoded header is checked before the decoder allocates a pixel buffer.
                    std::unique_ptr<OwnedPngImage> png(new (std::nothrow)
                                                           OwnedPngImage(data.get(), size));
                    if (!png) {
                        std::snprintf(failure, sizeof(failure),
                                      "Not enough memory for image descriptor");
                        return false;
                    }
                    data.release();
                    if (png->IsValid()) {
                        image = std::move(png);
                    }
                }
                if (!image) {
                    std::snprintf(failure, sizeof(failure), "Image decoding failed");
                    return false;
                }
                DisplayLockGuard lock(job->display);
                if (job->generation != job->display->visual_generation_.load())
                    return true;
                job->display->SetPreviewImage(std::move(image));
                ESP_LOGI(kTag, "Image submitted to TFT UI (%u bytes); verify physical display",
                         static_cast<unsigned>(size));
                return true;
            }();
            if (!success) {
                DisplayLockGuard lock(job->display);
                if (job->generation == job->display->visual_generation_.load()) {
                    ESP_LOGE(kTag, "Download failed: %s", failure);
                    job->display->SetPreviewImage(nullptr);
                    job->display->ShowNotification(failure, 10000);
                }
            }
        } catch (const std::exception& error) {
            ESP_LOGE(kTag, "Download failed: %s", error.what());
            DisplayLockGuard lock(job->display);
            if (job->generation == job->display->visual_generation_.load()) {
                job->display->SetPreviewImage(nullptr);
                job->display->ShowNotification("Image download failed", 10000);
            }
        }
        job->display->downloading_ = false;
    }
    vTaskDelete(nullptr);
}
#endif
