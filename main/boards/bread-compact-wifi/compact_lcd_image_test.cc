#include "compact_lcd_image_test.h"

#if CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST
#include "compact_tft_display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <exception>
#include <memory>

namespace {
constexpr char kTag[] = "LCD_IMAGE_TEST";
struct TestImage {
    const char* name;
    const char* url;
};
// Public EduHub illustrations, verified as baseline JPEG 160x128.
// These fixed test samples require no account, Robot token or lesson API.
constexpr TestImage kImages[] = {
    {"abide by",
     "https://media.coding.eduhubonline.com/english/word-images/"
     "toeic-illustration-v1-06a8ca8748748c525dc5fa2e19dc5a837cf3c87d989c0000273cf7641535afe5.jpg"},
    {"agreement",
     "https://media.coding.eduhubonline.com/english/word-images/"
     "toeic-illustration-v1-4ecc17238c902f27443ec16925206667368d9117b4475d047ba8136637012729.jpg"},
    {"assurance",
     "https://media.coding.eduhubonline.com/english/word-images/"
     "toeic-illustration-v1-6ca6274521c9d4ea3e2a1a29f7f3de3c51651880d2306f61d78ffd82d7eba3ea.jpg"},
    {"cancellation",
     "https://media.coding.eduhubonline.com/english/word-images/"
     "toeic-illustration-v1-6b1561107419dc0f1bc5369692d19bf8914213350a0c4d796e5dea38f5026d5c.jpg"},
    {"determine",
     "https://media.coding.eduhubonline.com/english/word-images/"
     "toeic-illustration-v1-e1aa0855ae8fd6183cc86b6e76dde1ebb5961ae4bebb8366f4f440ada69eaad6.jpg"},
};
static_assert(CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_COUNT >= 1 &&
              CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_COUNT <=
                  sizeof(kImages) / sizeof(kImages[0]));

void ImageTestTask(void* arg) {
    auto* display = static_cast<CompactTftDisplay*>(arg);
#if CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_COLORS
    ESP_LOGI(kTag, "STAGE 2: LVGL text LCD TEST ABC (2 seconds), check upright left-to-right text");
    display->ShowText("LCD TEST ABC");
    vTaskDelay(pdMS_TO_TICKS(2000));
#endif
    ESP_LOGI(kTag, "Public EduHub image test: %d images, %d seconds each; waiting for Wi-Fi",
             CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_COUNT,
             CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_SECONDS);
    unsigned index = 0;
    while (true) {
        if (!WifiManager::GetInstance().IsConnected() || display->IsDownloading()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        try {
            ESP_LOGI(kTag, "Loading image %u/%d: %s", index + 1,
                     CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_COUNT, kImages[index].name);
            if (!display->DownloadImage(kImages[index].url)) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            // Hold time starts after the download/decode finishes.
            while (display->IsDownloading())
                vTaskDelay(pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_SECONDS * 1000));
        } catch (const std::exception& error) {
            ESP_LOGE(kTag, "Cannot load test image: %s", error.what());
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        index = (index + 1) % CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_COUNT;
    }
}
}  // namespace

void RunCompactLcdColorTest(esp_lcd_panel_io_handle_t panel_io, int width, int height) {
#if CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_COLORS
    if (!panel_io || width <= 0 || width > 480 || height <= 0 || height > 480) {
        ESP_LOGE(kTag, "Invalid direct LCD diagnostic dimensions");
        return;
    }
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> row(
        static_cast<uint8_t*>(heap_caps_malloc(width * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
        heap_caps_free);
    if (!row) {
        ESP_LOGE(kTag, "Cannot allocate direct LCD diagnostic row");
        return;
    }
    struct Color {
        const char* name;
        uint16_t rgb565;
    };
    constexpr Color colors[] = {
        {"RED", 0xf800}, {"GREEN", 0x07e0}, {"BLUE", 0x001f}, {"WHITE", 0xffff}, {"BLACK", 0x0000}};
    const uint8_t columns[] = {0, 0, static_cast<uint8_t>((width - 1) >> 8),
                               static_cast<uint8_t>(width - 1)};
    // Synchronous command/parameter writes deliberately bypass the LVGL/DMA flush path.
    // No asynchronous color transfer references this row after it is freed.
    for (const auto& color : colors) {
        ESP_LOGI(kTag, "STAGE 1: direct LCD %s (%dx%d), no LVGL/JPEG/network", color.name, width,
                 height);
        for (int x = 0; x < width; ++x) {
            row.get()[x * 2] = color.rgb565 >> 8;
            row.get()[x * 2 + 1] = color.rgb565 & 0xff;
        }
        for (int y = 0; y < height; ++y) {
            const uint8_t rows[] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
                                    static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y)};
            esp_err_t result = esp_lcd_panel_io_tx_param(panel_io, 0x2a, columns, sizeof(columns));
            if (result == ESP_OK)
                result = esp_lcd_panel_io_tx_param(panel_io, 0x2b, rows, sizeof(rows));
            if (result == ESP_OK)
                result = esp_lcd_panel_io_tx_param(panel_io, 0x2c, row.get(), width * 2);
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "Direct LCD %s failed at row %d: %s", color.name, y,
                         esp_err_to_name(result));
                return;
            }
            if ((y & 15) == 15)
                vTaskDelay(pdMS_TO_TICKS(1));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

void StartCompactLcdImageTest(CompactTftDisplay* display) {
    if (display == nullptr ||
        xTaskCreate(ImageTestTask, "lcd_image_test", 4096, display, 2, nullptr) != pdPASS)
        ESP_LOGE(kTag, "Could not start LCD image test");
}
#endif
