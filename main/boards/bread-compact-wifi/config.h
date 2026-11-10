#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/uart.h>

#if CONFIG_BREAD_COMPACT_WIFI_USE_LEGACY_MIC_PINS
#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#else
// Shared-clock duplex uses the microphone's proven 16 kHz hardware rate.
// Playback is resampled to the same rate because BCLK/WS are shared.
#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000
#endif

// The menuconfig flag selects either legacy separate clocks or current shared clocks.
#if CONFIG_BREAD_COMPACT_WIFI_USE_LEGACY_MIC_PINS
#define AUDIO_I2S_METHOD_SIMPLEX
#else
#define AUDIO_I2S_METHOD_DUPLEX
#endif

#ifdef AUDIO_I2S_METHOD_SIMPLEX

// Legacy microphone wiring: WS=GPIO12, BCLK/SCK=GPIO14, DATA_OUT/DIN=GPIO13.
#define AUDIO_I2S_MIC_GPIO_WS GPIO_NUM_12
#define AUDIO_I2S_MIC_GPIO_SCK GPIO_NUM_14
#define AUDIO_I2S_MIC_GPIO_DIN GPIO_NUM_13
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_21
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_47
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_48

#else

// Full duplex: BCLK/WS are shared, while microphone DIN and speaker DOUT
// remain separate. Both directions run at 16 kHz on the same I2S controller.
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_47
#define AUDIO_I2S_GPIO_WS GPIO_NUM_48
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_45
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_21

#endif

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BUTTON_A_GPIO GPIO_NUM_19
#define BUTTON_B_GPIO GPIO_NUM_20

#define DISPLAY_SDA_PIN GPIO_NUM_8
#define DISPLAY_SCL_PIN GPIO_NUM_9

#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
#if CONFIG_BREAD_COMPACT_WIFI_USE_LEGACY_MIC_PINS && \
    !CONFIG_BREAD_COMPACT_WIFI_TFT_USE_CS38_RST40_DC41
#error "TFT pins 12/13/14 conflict with legacy microphone wiring"
#endif
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
// Physical 320x480 ILI9486 panel, rotated to landscape.
#define TFT_DISPLAY_WIDTH 480
#define TFT_DISPLAY_HEIGHT 320
#define TFT_DISPLAY_MIRROR_X true
#define TFT_DISPLAY_MIRROR_Y false
#define TFT_DISPLAY_SWAP_XY true
#define TFT_DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#elif CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9341_320X240
// Physical 240x320 ILI9341 3.2-inch SPI panel, rotated to landscape.
#define TFT_DISPLAY_WIDTH 320
#define TFT_DISPLAY_HEIGHT 240
// The installed 3.2-inch panel is upside down relative to native landscape.
// Toggle both axes together for 180-degree rotation without mirroring glyphs.
#define TFT_DISPLAY_MIRROR_X true
#define TFT_DISPLAY_MIRROR_Y true
#define TFT_DISPLAY_SWAP_XY true
#define TFT_DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#elif CONFIG_BREAD_COMPACT_WIFI_TFT_ST7789_320X240
// Physical 240x320 ST7789 panel, rotated to landscape.
#define TFT_DISPLAY_WIDTH 320
#define TFT_DISPLAY_HEIGHT 240
#define TFT_DISPLAY_MIRROR_X true
#define TFT_DISPLAY_MIRROR_Y false
#define TFT_DISPLAY_SWAP_XY true
#define TFT_DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#else
// The physical 128x160 panel is presented to LVGL in landscape orientation.
#define TFT_DISPLAY_WIDTH 160
#define TFT_DISPLAY_HEIGHT 128
// Keep landscape, rotate the mounted panel 180 degrees by flipping both axes.
#define TFT_DISPLAY_MIRROR_X false
#define TFT_DISPLAY_MIRROR_Y true
#define TFT_DISPLAY_SWAP_XY true
#define TFT_DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#endif
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ST7789_INVERT_COLOR || \
    CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9341_INVERT_COLOR
#define TFT_DISPLAY_INVERT_COLOR true
#else
#define TFT_DISPLAY_INVERT_COLOR false
#endif
#if CONFIG_BREAD_COMPACT_WIFI_TFT_USE_CS38_RST40_DC41
#define TFT_DISPLAY_CS_PIN GPIO_NUM_38
#define TFT_DISPLAY_RST_PIN GPIO_NUM_40
#define TFT_DISPLAY_DC_PIN GPIO_NUM_41
#else
#define TFT_DISPLAY_CS_PIN GPIO_NUM_12
#define TFT_DISPLAY_RST_PIN GPIO_NUM_14
#define TFT_DISPLAY_DC_PIN GPIO_NUM_13
#endif
#define TFT_DISPLAY_MOSI_PIN GPIO_NUM_15
#define TFT_DISPLAY_CLK_PIN GPIO_NUM_2
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
#define SDCARD_CLK_PIN GPIO_NUM_10
#define SDCARD_CMD_PIN GPIO_NUM_11
#define SDCARD_D0_PIN GPIO_NUM_16
#endif
#endif

#define DISPLAY_WIDTH 128
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT 32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT 64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT 64
#define SH1106
#else
#error "OLED display type is not selected"
#endif

// UART camera link to the Sipeed K210. These pins are independent from the
// audio and display buses used by this board.
#define K210_CAMERA_UART_PORT UART_NUM_1
#define K210_CAMERA_UART_TX_PIN GPIO_NUM_17
#define K210_CAMERA_UART_RX_PIN GPIO_NUM_18
#define K210_CAMERA_UART_BAUD_RATE 115200

#endif  // _BOARD_CONFIG_H_
