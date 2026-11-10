#pragma once

#include "sdkconfig.h"

#if CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST
#include <esp_lcd_panel_io.h>

class CompactTftDisplay;

// An incremental build can still have the SDK header from before this option existed.
// Keep the Kconfig default until reconfigure regenerates that header.
#ifdef CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_SPI_MHZ
inline constexpr int kCompactLcdImageTestSpiMhz = CONFIG_BREAD_COMPACT_WIFI_LCD_IMAGE_TEST_SPI_MHZ;
#else
inline constexpr int kCompactLcdImageTestSpiMhz = 10;
#endif
static_assert(kCompactLcdImageTestSpiMhz >= 1 && kCompactLcdImageTestSpiMhz <= 20);

// Run before registering the panel with LVGL, after applying rotation and display-on.
void RunCompactLcdColorTest(esp_lcd_panel_io_handle_t panel_io, int width, int height);

// The board owns the display for the lifetime of this background task.
void StartCompactLcdImageTest(CompactTftDisplay* display);
#endif
