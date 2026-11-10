#include "sdkconfig.h"

#if CONFIG_BREAD_COMPACT_WIFI_ILI9486_SD_TEST
#include "compact_lcd_sd_test_pixels.h"
#include "compact_sd_gif.h"
#include "config.h"
#include "display/lvgl_display/gif/gifdec.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace {
constexpr char kTag[] = "LCD_SD_TEST";
constexpr unsigned kWidth = 480;
constexpr unsigned kHeight = 320;
spi_device_handle_t lcd = nullptr;
uint8_t* row = nullptr;
constexpr bool rgb666 = true;

void Pause(unsigned milliseconds) { vTaskDelay(pdMS_TO_TICKS(milliseconds)); }

void Transfer(const void* data, size_t size) {
    spi_transaction_t transaction{};
    transaction.tx_buffer = data;
    transaction.length = size * 8;
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &transaction));
}

void Command(uint8_t command, const void* data = nullptr, size_t size = 0) {
    ESP_ERROR_CHECK(gpio_set_level(TFT_DISPLAY_DC_PIN, 0));
    Transfer(&command, 1);
    esp_rom_delay_us(10);
    if (size > 0) {
        ESP_ERROR_CHECK(gpio_set_level(TFT_DISPLAY_DC_PIN, 1));
        Transfer(data, size);
    }
}

void Window(unsigned x, unsigned y, unsigned width, unsigned height) {
    const uint8_t columns[] = {uint8_t(x >> 8), uint8_t(x), uint8_t((x + width - 1) >> 8),
                               uint8_t(x + width - 1)};
    const uint8_t rows[] = {uint8_t(y >> 8), uint8_t(y), uint8_t((y + height - 1) >> 8),
                            uint8_t(y + height - 1)};
    Command(0x2A, columns, sizeof(columns));
    Command(0x2B, rows, sizeof(rows));
    Command(0x2C);
    ESP_ERROR_CHECK(gpio_set_level(TFT_DISPLAY_DC_PIN, 1));
}

void WriteRow(unsigned x, unsigned y, unsigned width, unsigned bytes) {
    const auto native = CompactLcdTestNativeWindow(x, y, width, 1);
    ESP_ERROR_CHECK(native.width && native.height && bytes == width * 3 ? ESP_OK
                                                                        : ESP_ERR_INVALID_ARG);
    // One logical row becomes one native column. Reissue its address window
    // explicitly instead of depending on cursor wrap across a large window.
    Window(native.x, native.y, native.width, native.height);
    Transfer(row, bytes);
}

void InitializeLcd() {
    gpio_config_t gpio{};
    gpio.pin_bit_mask = (1ULL << TFT_DISPLAY_RST_PIN) | (1ULL << TFT_DISPLAY_DC_PIN);
    gpio.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&gpio));
    spi_bus_config_t bus{};
    bus.mosi_io_num = TFT_DISPLAY_MOSI_PIN;
    bus.miso_io_num = GPIO_NUM_NC;
    bus.sclk_io_num = TFT_DISPLAY_CLK_PIN;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = kWidth * 3;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t device{};
    device.clock_speed_hz = CONFIG_BREAD_COMPACT_WIFI_LCD_TEST_SPI_MHZ * 1000000;
    device.mode = 0;
    device.spics_io_num = TFT_DISPLAY_CS_PIN;
    device.queue_size = 1;
    device.flags = SPI_DEVICE_HALFDUPLEX;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &device, &lcd));
    row = static_cast<uint8_t*>(heap_caps_malloc(kWidth * 3, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    ESP_ERROR_CHECK(row != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(gpio_set_level(TFT_DISPLAY_RST_PIN, 0));
    Pause(30);
    ESP_ERROR_CHECK(gpio_set_level(TFT_DISPLAY_RST_PIN, 1));
    Pause(150);
    // Native portrait addressing from Robot's main_emoji_display_test.c,
    // with software rotation and the RGB666 format confirmed on this module.
    Command(0x01);
    Pause(150);
    Command(0x11);
    Pause(150);
    const uint8_t colmod = 0x66;
    Command(0x3A, &colmod, 1);
    const uint8_t madctl = 0x48;  // MX | BGR, MV disabled.
    Command(0x36, &madctl, 1);
    Command(0x20);
    Command(0x29);
    Pause(100);
    ESP_LOGI(kTag, "Raw SPI3 %d MHz; CS%d RST%d DC%d MOSI%d SCK%d",
             CONFIG_BREAD_COMPACT_WIFI_LCD_TEST_SPI_MHZ, static_cast<int>(TFT_DISPLAY_CS_PIN),
             static_cast<int>(TFT_DISPLAY_RST_PIN), static_cast<int>(TFT_DISPLAY_DC_PIN),
             static_cast<int>(TFT_DISPLAY_MOSI_PIN), static_cast<int>(TFT_DISPLAY_CLK_PIN));
    ESP_LOGW(kTag,
             "RGB666 only; MADCTL=0x48; logical 480x320 -> native 320x480, software rotation");
}

void Fill(unsigned r, unsigned g, unsigned b) {
    unsigned bytes = 0;
    for (unsigned x = 0; x < kWidth; ++x)
        bytes += CompactLcdTestPixel(row + bytes, r, g, b, rgb666);
    for (unsigned y = 0; y < kHeight; ++y) {
        WriteRow(0, y, kWidth, bytes);
        if ((y & 15) == 15)
            Pause(1);
    }
}

void Patterns() {
    struct Color {
        const char* name;
        unsigned r, g, b;
    };
    constexpr Color colors[] = {{"RED", 255, 0, 0},
                                {"GREEN", 0, 255, 0},
                                {"BLUE", 0, 0, 255},
                                {"WHITE", 255, 255, 255},
                                {"BLACK", 0, 0, 0}};
    for (const auto& color : colors) {
        ESP_LOGI(kTag, "STAGE 1: solid %s, procedural pixels, no GIF/SD reads", color.name);
        Fill(color.r, color.g, color.b);
        Pause(2000);
    }
    ESP_LOGI(kTag, "STAGE 2: checkerboard, white border, red X=240 / green Y=160 center lines");
    for (unsigned y = 0; y < kHeight; ++y) {
        unsigned bytes = 0;
        for (unsigned x = 0; x < kWidth; ++x) {
            const bool white = x == 0 || y == 0 || x == kWidth - 1 || y == kHeight - 1 ||
                               (((x / 20) + (y / 20)) & 1);
            const unsigned value = white ? 255 : 0;
            const bool center_x = x == kWidth / 2;
            const bool center_y = y == kHeight / 2;
            bytes += CompactLcdTestPixel(row + bytes,
                                         center_x   ? 255
                                         : center_y ? 0
                                                    : value,
                                         center_y   ? 255
                                         : center_x ? 0
                                                    : value,
                                         center_x || center_y ? 0 : value, rgb666);
        }
        WriteRow(0, y, kWidth, bytes);
        Pause(1);
    }
    Pause(4000);
}

void DrawGif(const gd_GIF* gif, bool fit) {
    const auto size = fit ? CompactLcdTestFit(gif->width, gif->height, kWidth, kHeight)
                          : CompactLcdTestSize{gif->width, gif->height};
    const unsigned x0 = (kWidth - size.width) / 2;
    const unsigned y0 = (kHeight - size.height) / 2;
    ESP_LOGD(kTag, "GIF logical rectangle x=%u y=%u w=%u h=%u", x0, y0, size.width, size.height);
    for (unsigned y = 0; y < size.height; ++y) {
        unsigned bytes = 0;
        const unsigned source_y = y * gif->height / size.height;
        for (unsigned x = 0; x < size.width; ++x) {
            const unsigned source_x = x * gif->width / size.width;
            const auto* pixel = gif->canvas + (source_y * gif->width + source_x) * 4;
            const unsigned alpha = pixel[3];
            bytes += CompactLcdTestPixel(row + bytes, pixel[2] * alpha / 255,
                                         pixel[1] * alpha / 255, pixel[0] * alpha / 255, rgb666);
        }
        WriteRow(x0, y0 + y, size.width, bytes);
        if ((y & 15) == 15)
            Pause(1);
    }
}

void PlayGif(CompactSdGif& card, bool fit) {
    const std::string name = CONFIG_BREAD_COMPACT_WIFI_LCD_TEST_GIF;
    unsigned width = 0, height = 0;
    if (!card.Contains(name) || !card.ValidateFile(name, width, height)) {
        ESP_LOGE(kTag, "GIF missing/invalid: /sdcard/%s%s", CompactSdGif::kRelativePrefix,
                 name.c_str());
        return;
    }
    if (!fit && (width > kWidth || height > kHeight)) {
        ESP_LOGW(kTag, "Native-size test skipped: GIF exceeds the screen; fit test follows");
        return;
    }
    const std::string path = std::string("S:/") + CompactSdGif::kRelativePrefix + name;
    auto* gif = gd_open_gif_file(path.c_str());
    if (gif == nullptr) {
        ESP_LOGE(kTag, "Could not open GIF decoder: %s", path.c_str());
        return;
    }
    if (gif->width != width || gif->height != height) {
        ESP_LOGE(kTag, "GIF dimensions changed after validation");
        gd_close_gif(gif);
        return;
    }
    const auto size =
        fit ? CompactLcdTestFit(width, height, kWidth, kHeight) : CompactLcdTestSize{width, height};
    ESP_LOGI(kTag, "STAGE %d: SD GIF %s, %ux%u -> %ux%u, raw SPI, no LVGL display", fit ? 4 : 3,
             name.c_str(), width, height, size.width, size.height);
    const unsigned x = (kWidth - size.width) / 2;
    const unsigned y = (kHeight - size.height) / 2;
    const auto native = CompactLcdTestNativeWindow(x, y, size.width, size.height);
    ESP_LOGI(kTag, "GIF placement: logical (%u,%u) %ux%u; native (%u,%u) %ux%u", x, y, size.width,
             size.height, native.x, native.y, native.width, native.height);
    Fill(0, 0, 0);
    const int64_t end = esp_timer_get_time() + 8000000;
    while (esp_timer_get_time() < end) {
        const int result = gd_get_frame(gif);
        if (result < 0) {
            ESP_LOGE(kTag, "GIF frame decode failed");
            break;
        }
        if (result == 0) {
            gd_rewind(gif);
            Pause(20);
            continue;
        }
        gd_render_frame(gif, gif->canvas);
        DrawGif(gif, fit);
        Pause(std::max(20u, unsigned(gif->gce.delay) * 10));
    }
    gd_close_gif(gif);
}
}  // namespace

void RunCompactLcdSdTest() {
    ESP_LOGW(kTag, "DIAGNOSTIC MODE: assistant/audio/Wi-Fi/camera are not started");
    InitializeLcd();
    // Only filesystem and allocator services are used; no LVGL display exists.
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port));
    auto card = std::make_unique<CompactSdGif>();
    bool sd_started = false;
    for (;;) {
        ESP_LOGW(kTag, "FORMAT: RGB666, COLMOD=0x66, 3 bytes/pixel, native portrait windows");
        Patterns();
        if (!sd_started) {
            sd_started = true;
            card->Start();
            // Mount worker has bounded retries. LCD-only stages have
            // already completed before any SD activity starts.
            const int64_t deadline = esp_timer_get_time() + 30000000;
            while (card->IsInitializing() && esp_timer_get_time() < deadline)
                Pause(100);
        }
        if (card->IsReady()) {
            PlayGif(*card, false);
            PlayGif(*card, true);
        } else {
            ESP_LOGE(kTag, "SD unavailable: continuing LCD-only patterns");
        }
        Pause(2000);
    }
}
#endif
