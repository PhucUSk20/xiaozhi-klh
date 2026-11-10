#pragma once

#include "sdkconfig.h"

#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
#include <driver/spi_master.h>
#include <esp_lcd_io_spi.h>

// Robot-compatible SPI framing with RGB565-to-RGB666 color conversion.
// LVGL and screen tools retain their existing RGB565 rendering path.
esp_err_t NewCompactIli9486Io(spi_host_device_t bus, const esp_lcd_panel_io_spi_config_t& config,
                              esp_lcd_panel_io_handle_t* result);
#endif
