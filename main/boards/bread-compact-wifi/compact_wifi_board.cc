#include "boards/common/hub_music_player.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "wifi_board.h"
#if CONFIG_OLED_SH1106_128X64
#include "compact_oled_emoji_display.h"
#endif
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "compact_caro_board.h"
#include "compact_ili9486_io.h"
#include "compact_lcd_image_test.h"
#include "compact_tft_display.h"
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9341_320X240
#include "esp_lcd_ili9341.h"
#endif
#include "config.h"
#include "k210_uart_camera.h"
#include "mcp_server.h"
#include "robot_control_server.h"
#include "rock_paper_scissors_controller.h"
#include "system_reset.h"

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <wifi_manager.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

#ifdef AUDIO_I2S_METHOD_DUPLEX
class BreadCompactDuplexAudioCodec : public NoAudioCodec {
private:
    uint32_t input_debug_buffers_ = 0;
    uint8_t input_debug_logs_ = 0;
    int input_debug_left_peak_ = 0;
    int input_debug_right_peak_ = 0;
    int64_t input_debug_left_sum_ = 0;
    int64_t input_debug_right_sum_ = 0;
    uint32_t input_debug_left_clipped_ = 0;
    uint32_t input_debug_samples_ = 0;

protected:
    int Write(const int16_t* data, int samples) override {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        std::vector<int32_t> stereo_buffer(static_cast<size_t>(samples) * 2);
        const int32_t volume_factor = static_cast<int32_t>(
            (static_cast<int64_t>(output_volume_) * output_volume_ * 65536) / 10000);

        for (int i = 0; i < samples; ++i) {
            int64_t scaled = static_cast<int64_t>(data[i]) * volume_factor;
            int32_t sample = scaled > INT32_MAX   ? INT32_MAX
                             : scaled < INT32_MIN ? INT32_MIN
                                                  : static_cast<int32_t>(scaled);
            stereo_buffer[2 * i] = sample;
            stereo_buffer[2 * i + 1] = sample;
        }

        size_t bytes_written = 0;
        ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, stereo_buffer.data(),
                                          stereo_buffer.size() * sizeof(int32_t), &bytes_written,
                                          portMAX_DELAY));
        return static_cast<int>(bytes_written / (2 * sizeof(int32_t)));
    }

    int Read(int16_t* dest, int samples) override {
        std::vector<int32_t> stereo_buffer(static_cast<size_t>(samples) * 2);
        size_t bytes_read = 0;
        constexpr uint32_t kReadTimeoutMs = 200;
        if (i2s_channel_read(rx_handle_, stereo_buffer.data(),
                             stereo_buffer.size() * sizeof(int32_t), &bytes_read,
                             kReadTimeoutMs) != ESP_OK) {
            return 0;
        }

        const int frames = static_cast<int>(bytes_read / (2 * sizeof(int32_t)));
        int left_peak = 0;
        int right_peak = 0;
        int64_t left_sum = 0;
        int64_t right_sum = 0;
        uint32_t left_clipped = 0;
        for (int i = 0; i < frames; ++i) {
            const int32_t left_value =
                stereo_buffer[2 * i] >> CONFIG_BREAD_COMPACT_WIFI_SPH0645_PCM_SHIFT;
            const int32_t right_value =
                stereo_buffer[2 * i + 1] >> CONFIG_BREAD_COMPACT_WIFI_SPH0645_PCM_SHIFT;
            const int16_t left_sample = left_value > INT16_MAX ? INT16_MAX
                                        : left_value < -INT16_MAX
                                            ? -INT16_MAX
                                            : static_cast<int16_t>(left_value);
            const int16_t right_sample = right_value > INT16_MAX ? INT16_MAX
                                         : right_value < -INT16_MAX
                                             ? -INT16_MAX
                                             : static_cast<int16_t>(right_value);
            dest[i] = left_sample;

            const int left_magnitude =
                left_sample < 0 ? -static_cast<int>(left_sample) : left_sample;
            const int right_magnitude =
                right_sample < 0 ? -static_cast<int>(right_sample) : right_sample;
            if (left_magnitude > left_peak)
                left_peak = left_magnitude;
            if (right_magnitude > right_peak)
                right_peak = right_magnitude;
            left_sum += left_magnitude;
            right_sum += right_magnitude;
            if (left_magnitude == INT16_MAX)
                ++left_clipped;
        }

        if (left_peak > input_debug_left_peak_)
            input_debug_left_peak_ = left_peak;
        if (right_peak > input_debug_right_peak_)
            input_debug_right_peak_ = right_peak;
        input_debug_left_sum_ += left_sum;
        input_debug_right_sum_ += right_sum;
        input_debug_left_clipped_ += left_clipped;
        input_debug_samples_ += frames;
        if (++input_debug_buffers_ >= 500 && input_debug_logs_ < 3) {
            input_debug_buffers_ = 0;
            ++input_debug_logs_;
            const int left_average =
                input_debug_samples_ > 0
                    ? static_cast<int>(input_debug_left_sum_ / input_debug_samples_)
                    : 0;
            const int right_average =
                input_debug_samples_ > 0
                    ? static_cast<int>(input_debug_right_sum_ / input_debug_samples_)
                    : 0;
            ESP_LOGI(TAG, "Microphone PCM L peak=%d avg=%d clipped=%lu/%lu; R peak=%d avg=%d",
                     input_debug_left_peak_, left_average,
                     static_cast<unsigned long>(input_debug_left_clipped_),
                     static_cast<unsigned long>(input_debug_samples_), input_debug_right_peak_,
                     right_average);
            input_debug_left_peak_ = 0;
            input_debug_right_peak_ = 0;
            input_debug_left_sum_ = 0;
            input_debug_right_sum_ = 0;
            input_debug_left_clipped_ = 0;
            input_debug_samples_ = 0;
        }
        return frames;
    }

public:
    void Start() override {
        NoAudioCodec::Start();
        // Start quietly even when NVS contains a louder volume from an earlier session.
        SetOutputVolume(30);
    }

    BreadCompactDuplexAudioCodec(int sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout,
                                 gpio_num_t din) {
        duplex_ = true;
        input_sample_rate_ = sample_rate;
        output_sample_rate_ = sample_rate;

        i2s_chan_config_t channel_config = {
            .id = XIAOZHI_I2S_PORT(0),
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
        };
        ESP_ERROR_CHECK(i2s_new_channel(&channel_config, &tx_handle_, &rx_handle_));

        i2s_std_config_t std_config = {
            .clk_cfg =
                {
                    .sample_rate_hz = static_cast<uint32_t>(sample_rate),
                    .clk_src = I2S_CLK_SRC_DEFAULT,
                    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
                    .ext_clk_freq_hz = 0,
#endif
                },
            .slot_cfg =
                {
                    .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
                    .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                    .slot_mode = I2S_SLOT_MODE_STEREO,
                    .slot_mask = I2S_STD_SLOT_BOTH,
                    .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
                    .ws_pol = false,
                    .bit_shift = true,
#ifdef I2S_HW_VERSION_2
                    .left_align = true,
                    .big_endian = false,
                    .bit_order_lsb = false,
#endif
                },
            .gpio_cfg =
                {
                    .mclk = I2S_GPIO_UNUSED,
                    .bclk = bclk,
                    .ws = ws,
                    .dout = dout,
                    .din = din,
                    .invert_flags =
                        {
                            .mclk_inv = false,
                            .bclk_inv = false,
                            .ws_inv = false,
                        },
                },
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_config));
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_config));
        ESP_LOGI(TAG,
                 "Stereo duplex I2S created: TX duplicated to L/R, microphone RX=LEFT, PCM "
                 "shift=%d",
                 CONFIG_BREAD_COMPACT_WIFI_SPH0645_PCM_SHIFT);
    }
};
#endif  // AUDIO_I2S_METHOD_DUPLEX

class CompactWifiBoard : public WifiBoard {
private:
    static constexpr uint32_t kControlQrDisplayDurationMs = 15000;

    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
    std::unique_ptr<CompactTftDisplay> tft_display_;
#endif
    Button boot_button_;
    Button button_a_;
    Button button_b_;
    std::unique_ptr<RockPaperScissorsController> rock_paper_scissors_;
    std::unique_ptr<RobotControlServer> robot_control_server_;
    std::unique_ptr<K210UartCamera> camera_;

    void RegisterLocalControlTool() {
        McpServer::GetInstance().AddTool(
            "self.robot.open_local_control",
            "Open the robot's private LAN control page. Call this when the user asks to open, "
            "show, or access robot controls. The robot displays a short-lived QR code that the "
            "user scans from a phone or tablet connected to the same Wi-Fi network.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (robot_control_server_ == nullptr || !robot_control_server_->IsRunning()) {
                    return std::string(
                        "Local control is unavailable. Connect the robot to Wi-Fi first.");
                }
                const std::string url = robot_control_server_->CreateAccessSession(600);
                if (url.empty()) {
                    return std::string("Could not create the local control session.");
                }
#if CONFIG_OLED_SH1106_128X64
                if (auto* emoji_display = dynamic_cast<CompactOledEmojiDisplay*>(display_)) {
                    if (!emoji_display->ShowControlQr(url, kControlQrDisplayDurationMs)) {
                        return std::string(
                            "The local control session was created, but its QR code could not be "
                            "displayed.");
                    }
                } else {
                    ESP_LOGE(TAG, "Emoji display is unavailable for the local control QR code");
                    return std::string(
                        "The local control session was created, but the emoji display is "
                        "unavailable.");
                }
#else
                display_->ShowNotification(url, 10000);
#endif
                return std::string(
                    "The local control QR code is on the robot display. It expires in 10 minutes.");
            });
    }

    void ShowLocalControlQrFromButton() {
        if (robot_control_server_ == nullptr || !robot_control_server_->IsRunning()) {
            GetDisplay()->ShowNotification("Connect the robot to Wi-Fi first.");
            return;
        }
        const std::string url = robot_control_server_->CreateAccessSession(600);
        if (url.empty()) {
            GetDisplay()->ShowNotification("Could not create the control QR code.");
            return;
        }
#if CONFIG_OLED_SH1106_128X64
        if (auto* emoji_display = dynamic_cast<CompactOledEmojiDisplay*>(display_)) {
            if (emoji_display->ShowControlQr(url, kControlQrDisplayDurationMs))
                return;
        }
#endif
        ESP_LOGE(TAG, "Could not display the local control QR code from Button B");
        GetDisplay()->ShowNotification("QR display is unavailable.");
    }

    std::string GetWifiConfigSsidPrefix() const override { return "KLeaderBit"; }

    void OnWifiConfigModeStarted(const std::string& ssid) override {
        if (robot_control_server_ != nullptr) {
            if (!robot_control_server_->Start(81)) {
                ESP_LOGE(TAG, "Failed to start robot control server for configuration AP");
            }
        }
#if CONFIG_BREAD_COMPACT_WIFI_SH1106_EMOJI_ONLY
        if (auto* emoji_display = dynamic_cast<CompactOledEmojiDisplay*>(display_)) {
            emoji_display->ShowWifiConfig(ssid);
            return;
        }
#endif
        WifiBoard::OnWifiConfigModeStarted(ssid);
    }

    void OnWifiStationConnected() override {
        bool control_server_started = false;
        if (robot_control_server_ != nullptr) {
            robot_control_server_->Stop();
            control_server_started = robot_control_server_->Start(80);
        }
#if CONFIG_BREAD_COMPACT_WIFI_SH1106_EMOJI_ONLY
        if (auto* emoji_display = dynamic_cast<CompactOledEmojiDisplay*>(display_)) {
            emoji_display->ShowClockMode();
        }
#endif
        if (control_server_started) {
            const std::string ip = WifiManager::GetInstance().GetIpAddress();
            if (!ip.empty()) {
                display_->ShowNotification("Web: http://" + ip, 6000);
            }
        }
    }

    void OnWifiStationDisconnected() override {
        if (robot_control_server_ != nullptr) {
            robot_control_server_->Stop();
        }
    }

    void EndCaroGame() {
#if CONFIG_OLED_SH1106_128X64
        if (auto* oled = dynamic_cast<CompactOledEmojiDisplay*>(display_)) {
            oled->EndCaroBoard();
        }
#endif
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
        if (tft_display_) {
            tft_display_->EndCaroBoard();
        }
#endif
    }

    void RegisterCaroTool() {
#if CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST
        return;  // The diagnostic slideshow owns the TFT.
#endif
        bool has_screen = false;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
        has_screen = tft_display_ != nullptr;
#endif
#if CONFIG_OLED_SH1106_128X64
        has_screen = has_screen || dynamic_cast<CompactOledEmojiDisplay*>(display_) != nullptr;
#endif
        if (!has_screen) {
            return;
        }
        McpServer::GetInstance().AddTool(
            "self.screen.show_caro",
            "Play voice tic-tac-toe (caro 3x3) on the robot screen. The assistant manages turns "
            "and win/draw rules. Send cells as exactly 9 characters in reading order: "
            "each empty square keeps its number 1-9, taken squares contain X or O. "
            "Start with 123456789. Example: O234X6789. Update after every move. "
            "Send cells=end when the user ends the game to restore the normal screen.",
            PropertyList({Property("cells", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                const auto cells = properties["cells"].value<std::string>();
                if (cells != "end" && !IsValidCompactCaroBoard(cells)) {
                    throw std::runtime_error(
                        "cells must contain 9 squares: their own digit, X or O");
                }
                if (cells == "end") {
                    EndCaroGame();
                    return std::string("Caro board closed.");
                }
                if (rock_paper_scissors_ && rock_paper_scissors_->IsRunning()) {
                    throw std::runtime_error("Wait for Rock Paper Scissors to finish first");
                }
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
                if (tft_display_) {
                    tft_display_->ShowCaroBoard(cells);
                    return std::string("Caro board updated: ") + cells;
                }
#endif
#if CONFIG_OLED_SH1106_128X64
                if (auto* oled = dynamic_cast<CompactOledEmojiDisplay*>(display_)) {
                    oled->ShowCaroBoard(cells);
                    return std::string("Caro board updated: ") + cells;
                }
#endif
                throw std::runtime_error("No Caro-capable screen is ready");
            });
    }

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
    void InitializeTftDisplay() {
        spi_bus_config_t bus_config = {};
        bus_config.mosi_io_num = TFT_DISPLAY_MOSI_PIN;
        bus_config.miso_io_num = GPIO_NUM_NC;
        bus_config.sclk_io_num = TFT_DISPLAY_CLK_PIN;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz = TFT_DISPLAY_WIDTH * TFT_DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = TFT_DISPLAY_CS_PIN;
        io_config.dc_gpio_num = TFT_DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
#ifndef CONFIG_BREAD_COMPACT_WIFI_TFT_SPI_MHZ
#define CONFIG_BREAD_COMPACT_WIFI_TFT_SPI_MHZ 10
#endif
        io_config.pclk_hz = CONFIG_BREAD_COMPACT_WIFI_TFT_SPI_MHZ * 1000 * 1000;
#if CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST
        io_config.pclk_hz = kCompactLcdImageTestSpiMhz * 1000 * 1000;
        ESP_LOGI(TAG, "LCD image diagnostic overrides TFT SPI clock to %d MHz",
                 kCompactLcdImageTestSpiMhz);
#endif
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
        ESP_ERROR_CHECK(NewCompactIli9486Io(SPI3_HOST, io_config, &panel_io));
        ESP_LOGI(TAG, "ILI9486 IO: Robot polling SPI, RGB565 -> RGB666 (3 bytes/pixel)");
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));
#endif
        ESP_LOGI(TAG,
                 "TFT SPI configured: %u Hz (%u MHz), mode %d; "
                 "SCK=%d MOSI=%d CS=%d DC=%d RST=%d; backlight GPIO not configured",
                 static_cast<unsigned>(io_config.pclk_hz),
                 static_cast<unsigned>(io_config.pclk_hz / 1000000), io_config.spi_mode,
                 static_cast<int>(TFT_DISPLAY_CLK_PIN), static_cast<int>(TFT_DISPLAY_MOSI_PIN),
                 static_cast<int>(TFT_DISPLAY_CS_PIN), static_cast<int>(TFT_DISPLAY_DC_PIN),
                 static_cast<int>(TFT_DISPLAY_RST_PIN));

        // Keep the base driver's input at RGB565 (two bytes/pixel). The ILI9486
        // IO adapter converts that input to RGB666; setting this to 18 would
        // make the base driver overread LVGL's two-byte input buffer.
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = TFT_DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = TFT_DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9341_320X240
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG,
                 "ILI9341 3.2-inch SPI: 320x240, RGB565, BGR; swap_xy=%d mirror_x=%d mirror_y=%d",
                 TFT_DISPLAY_SWAP_XY, TFT_DISPLAY_MIRROR_X, TFT_DISPLAY_MIRROR_Y);
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
        // Match the minimal init proven by compact_lcd_sd_test.cc on this
        // module. Robot's extra power/timing commands were not part of that
        // successful test. Do not run ST7789 default init afterward either.
        struct InitCommand {
            uint8_t command;
            uint8_t data[16];
            uint8_t size;
            uint16_t delay_ms;
        };
        static constexpr InitCommand commands[] = {
            {0x01, {0}, 0, 150},
            {0x11, {0}, 0, 150},
            {0x3A, {0x66}, 1, 10},  // Hardware test confirmed RGB666 on this SPI panel.
            // MX | MV | BGR, exactly as in the working landscape diagnostic.
            // LVGL subsequently reapplies these same swap/mirror settings.
            {0x36, {0x68}, 1, 10},
            {0x20, {0}, 0, 10},
            {0x29, {0}, 0, 100},
        };
        for (const auto& command : commands) {
            ESP_ERROR_CHECK(
                esp_lcd_panel_io_tx_param(panel_io, command.command, command.data, command.size));
            vTaskDelay(pdMS_TO_TICKS(command.delay_ms));
        }
        ESP_LOGI(TAG, "ILI9486 diagnostic-matched minimal init: 480x320, MADCTL=0x68, COLMOD=0x66");
#else
        // Use the selected driver's native init (ILI9341 or ST7789/ST7735-compatible).
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
#endif
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, TFT_DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
#if CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_COLORS
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, TFT_DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, TFT_DISPLAY_MIRROR_X, TFT_DISPLAY_MIRROR_Y));
        RunCompactLcdColorTest(panel_io, TFT_DISPLAY_WIDTH, TFT_DISPLAY_HEIGHT);
#endif
        const bool initialize_lvgl = dynamic_cast<LvglDisplay*>(display_) == nullptr;
        tft_display_ = std::make_unique<CompactTftDisplay>(
            panel_io, panel, TFT_DISPLAY_WIDTH, TFT_DISPLAY_HEIGHT, TFT_DISPLAY_MIRROR_X,
            TFT_DISPLAY_MIRROR_Y, TFT_DISPLAY_SWAP_XY, initialize_lvgl);
        tft_display_->SetupUI();
    }

    void RegisterTftTools() {
        auto* display = tft_display_.get();
        auto& mcp = McpServer::GetInstance();
        mcp.SetBeforeToolCall([display](const std::string& name) {
            // A device action suspends the card; screen tools control their own mode.
            if (name.compare(0, 12, "self.screen.") != 0) {
                display->ShowEmoji();
            }
        });
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
        mcp.AddTool("self.screen.list_sd_gifs",
                    "List GIF files on the SD card for the secondary TFT.", PropertyList(),
                    [display](const PropertyList&) -> ReturnValue {
                        const auto* card = display->SdCard();
                        cJSON* json = cJSON_CreateObject();
                        cJSON_AddBoolToObject(json, "ready", card && card->IsReady());
                        cJSON_AddBoolToObject(json, "mounting", card && card->IsInitializing());
                        cJSON* files = cJSON_AddArrayToObject(json, "files");
                        if (card) {
                            for (size_t i = 0; i < card->Count(); ++i) {
                                cJSON_AddItemToArray(files, cJSON_CreateString(card->Name(i)));
                            }
                        }
                        return json;
                    });
        mcp.AddTool(
            "self.screen.play_sd_gif",
            "Play an animated GIF from the SD card on the secondary TFT. "
            "Use a filename returned by list_sd_gifs, for example happy.gif. "
            "SH1106 emoji behavior is unchanged.",
            PropertyList({Property("name", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ReturnValue {
                if (!display->SdCard() || !display->SdCard()->IsReady()) {
                    return std::string("SD card is not ready. Check FAT32 format and wiring.");
                }
                if (!display->PlaySdGif(properties["name"].value<std::string>())) {
                    return std::string("GIF not indexed, or another image operation is busy.");
                }
                return std::string("Loading SD GIF on TFT.");
            });
#endif
#if CONFIG_BREAD_COMPACT_WIFI_TFT_WORD_IMAGES_SD
        ESP_LOGI(TAG, "Vocabulary image source: SD (/sdcard/manifest.json); HTTP disabled");
        mcp.AddTool(
            "self.screen.show_word_image",
            "Display a course vocabulary image directly from SD. Before EACH new course card, "
            "call this with exact lessonId and full key from nextWord/saveTarget. "
            "Do NOT call english_get_word_image or HTTP image tools in SD mode. "
            "Call silently before speaking; do not repeat lesson speech after the result. "
            "The exported ZIP must be extracted to SD root. No network fallback.",
            PropertyList(
                {Property("lessonId", kPropertyTypeString), Property("key", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ReturnValue {
                if (!display->ShowWordImage(properties["lessonId"].value<std::string>(),
                                            properties["key"].value<std::string>()))
                    throw std::runtime_error(
                        "Image worker is busy; continue the lesson without retrying speech");
                return std::string(
                    "SD image load started; not yet display confirmation. "
                    "Continue only unsaid speech; do not restart the lesson.");
            });
#else
        ESP_LOGI(TAG, "Vocabulary image source: HTTP/S3");
        mcp.AddTool(
            "self.screen.show_image",
            "Display a PNG or baseline JPEG from an HTTP/HTTPS URL on the TFT. "
            "Download runs in the background. Use images up to "
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
            "480x480 pixels, preferably 480x320 landscape, and "
#elif CONFIG_BREAD_COMPACT_WIFI_TFT_ST7789_320X240 || CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9341_320X240
            "320x320 pixels, preferably 320x240 landscape, and "
#else
            "320x320 pixels, preferably 160x128 landscape, and "
#endif
            "128 KiB. After english_get_word_image, pass its "
            "imageUrl as url. A started download does not confirm the image is displayed. "
            "The image stays until replaced or hidden.",
            PropertyList({Property("url", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ReturnValue {
                if (!display->DownloadImage(properties["url"].value<std::string>())) {
                    return std::string("An image download is already in progress.");
                }
                return std::string("Image download started. Failures are shown on the screen.");
            });
#endif
        mcp.AddTool(
            "self.screen.show_text",
            "Immediately display an English vocabulary word or phrase on TFT. "
            "Use text (1..80 ASCII characters); no image URL, download or S3 required. "
            "Call silently BEFORE lesson speech, then speak the lesson ONCE. "
            "If already spoken, do not repeat it after the tool result; wait for the learner. "
            "Keep visible while awaiting repetition. Call self.screen.show_emoji "
            "when leaving vocabulary practice or returning to ordinary conversation.",
            PropertyList({Property("text", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ReturnValue {
                if (!display->ShowText(properties["text"].value<std::string>())) {
                    throw std::runtime_error("TFT text display is not ready");
                }
                return std::string(
                    "Text displayed. Continue only speech not yet spoken; "
                    "do not restart or repeat the lesson. If the repetition "
                    "request was already spoken, wait for the learner.");
            });
        mcp.AddTool(
            "self.screen.show_emoji",
            "Leave vocabulary text/image mode and resume SD emoji for normal chat. "
            "Call BEFORE replying to off-topic speech, jokes or requests for device actions, "
            "even while awaiting a word repetition. Suspend the word without marking it complete. "
            "Keep the card for repetitions, retries and questions about that word; "
            "redisplay the same unfinished word only when practice resumes.",
            PropertyList(), [display](const PropertyList&) -> ReturnValue {
                display->ShowEmoji();
                return true;
            });
        mcp.AddTool("self.screen.hide_image",
                    "Leave vocabulary text/image mode and resume SD emoji on TFT.", PropertyList(),
                    [display](const PropertyList&) -> ReturnValue {
                        display->ShowEmoji();
                        return true;
                    });
    }
#endif
    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags =
                {
                    .dc_low_on_data = 0,
                    .disable_control_phase = 0,
                },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

#if CONFIG_OLED_SH1106_128X64
#if CONFIG_BREAD_COMPACT_WIFI_SH1106_EMOJI_ONLY
        constexpr bool kEmojiOnly = true;
#else
        constexpr bool kEmojiOnly = false;
#endif
        display_ = new CompactOledEmojiDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                               DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, kEmojiOnly);
#else
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#endif
    }

    void AdjustButtonVolume(int direction) {
        auto* codec = GetAudioCodec();
        if (!codec)
            return;
#ifdef CONFIG_BREAD_COMPACT_WIFI_BUTTON_VOLUME_STEP
        constexpr int step = CONFIG_BREAD_COMPACT_WIFI_BUTTON_VOLUME_STEP;
#else
        constexpr int step = 10;  // Compatibility with SDK headers generated before the option.
#endif
        const int volume = std::clamp(codec->output_volume() + direction * step, 0, 100);
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        ESP_LOGI(TAG, "Button speaker volume: %d%%", volume);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        // Defaults retain the original actions; all volume changes run on the application task.
        button_a_.OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
#if CONFIG_BREAD_COMPACT_WIFI_BUTTON_A_VOLUME_UP
                AdjustButtonVolume(1);
#elif CONFIG_BREAD_COMPACT_WIFI_BUTTON_A_VOLUME_DOWN
                AdjustButtonVolume(-1);
#else
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
                if (tft_display_ != nullptr &&
                    Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    tft_display_->CycleIdleEmoji();
                    return;
                }
#endif
                if (rock_paper_scissors_ != nullptr) {
                    rock_paper_scissors_->StartGame(true);
                }
#endif
            });
        });
        button_a_.OnLongPress([this]() {
            if (rock_paper_scissors_ != nullptr) {
                rock_paper_scissors_->StopGame();
            }
        });

        // Button B action is selected in menuconfig.
        button_b_.OnClick([this]() {
#if CONFIG_BREAD_COMPACT_WIFI_BUTTON_B_VOLUME_UP
            Application::GetInstance().Schedule([this]() { AdjustButtonVolume(1); });
#elif CONFIG_BREAD_COMPACT_WIFI_BUTTON_B_VOLUME_DOWN
            Application::GetInstance().Schedule([this]() { AdjustButtonVolume(-1); });
#elif CONFIG_BREAD_COMPACT_WIFI_BUTTON_B_WAKE_WORD
            Application::GetInstance().WakeWordInvoke(
                CONFIG_BREAD_COMPACT_WIFI_BUTTON_B_WAKE_WORD_TEXT);
#else
            Application::GetInstance().Schedule([this]() { ShowLocalControlQrFromButton(); });
#endif
        });
    }

public:
    CompactWifiBoard()
        : boot_button_(BOOT_BUTTON_GPIO),
          button_a_(BUTTON_A_GPIO, false, 2000),
          button_b_(BUTTON_B_GPIO, false, 1000) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
        InitializeTftDisplay();
#if CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST
        StartCompactLcdImageTest(tft_display_.get());
#else
        RegisterTftTools();
#endif
#endif
        InitializeButtons();
        CompactOledEmojiDisplay* emoji_display = nullptr;
#if CONFIG_BREAD_COMPACT_WIFI_SH1106_EMOJI_ONLY
        emoji_display = dynamic_cast<CompactOledEmojiDisplay*>(display_);
#endif
        CompactTftDisplay* game_tft = nullptr;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
        game_tft = tft_display_.get();
#endif
        rock_paper_scissors_ = std::make_unique<RockPaperScissorsController>(
            display_i2c_bus_, emoji_display, game_tft);
        robot_control_server_ = std::make_unique<RobotControlServer>(*rock_paper_scissors_);
        camera_ =
            std::make_unique<K210UartCamera>(K210_CAMERA_UART_PORT, K210_CAMERA_UART_TX_PIN,
                                             K210_CAMERA_UART_RX_PIN, K210_CAMERA_UART_BAUD_RATE);
        RegisterHubMusicTools();
        RegisterLocalControlTool();
        RegisterCaroTool();
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_BOTH,
                                               AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS,
                                               AUDIO_I2S_MIC_GPIO_DIN, I2S_STD_SLOT_LEFT);
#else
        // Use an identical stereo frame for the paired TX/RX hardware. The
        // codec duplicates mono playback to both slots and extracts LEFT from
        // the interleaved microphone input.
        static BreadCompactDuplexAudioCodec audio_codec(AUDIO_OUTPUT_SAMPLE_RATE,
                                                        AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                                        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    void OnSessionIdle() override { EndCaroGame(); }

    void OnDeviceStateChanged(DeviceState state) override {
        if (state == kDeviceStateListening)
            StopHubMusic();
        if (state == kDeviceStateIdle)
            StartHubMusicBootTest();
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
        if (tft_display_)
            tft_display_->SetRuntimeState(state);
#endif
    }

    void OnEmotionChanged(const char* emotion) override {
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
        if (tft_display_)
            tft_display_->SetEmotion(emotion);
#endif
    }

    virtual Camera* GetCamera() override { return camera_.get(); }

    void OnWakeWordDetected(const std::string&) override {
        StopHubMusic();
        if (rock_paper_scissors_ != nullptr) {
            rock_paper_scissors_->InterruptForWakeWord();
            rock_paper_scissors_->StopMovement();
        }
    }
};

DECLARE_BOARD(CompactWifiBoard);
