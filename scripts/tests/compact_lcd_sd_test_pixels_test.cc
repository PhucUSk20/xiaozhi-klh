#include "../../main/boards/bread-compact-wifi/compact_lcd_sd_test_pixels.h"

constexpr bool CheckPixels() {
    unsigned char bytes[3]{};
    if (CompactLcdTestPixel(bytes, 255, 0, 0, false) != 2 || bytes[0] != 0xF8 || bytes[1] != 0)
        return false;
    if (CompactLcdTestPixel(bytes, 0, 255, 0, false) != 2 || bytes[0] != 0x07 || bytes[1] != 0xE0)
        return false;
    if (CompactLcdTestPixel(bytes, 0, 0, 255, false) != 2 || bytes[0] != 0 || bytes[1] != 0x1F)
        return false;
    if (CompactLcdTestPixel(bytes, 255, 127, 3, true) != 3 || bytes[0] != 0xFC ||
        bytes[1] != 0x7C || bytes[2] != 0)
        return false;
    const auto small = CompactLcdTestFit(160, 128, 480, 320);
    const auto wide = CompactLcdTestFit(480, 240, 480, 320);
    const auto portrait = CompactLcdTestFit(320, 480, 480, 320);
    const auto invalid = CompactLcdTestFit(0, 128, 480, 320);
    return small.width == 400 && small.height == 320 && wide.width == 480 && wide.height == 240 &&
           portrait.width == 213 && portrait.height == 320 && invalid.width == 0 &&
           invalid.height == 0;
}
static_assert(CheckPixels(), "LCD diagnostic pixel encoding/scale calculation failed");

constexpr auto full = CompactLcdTestNativeWindow(0, 0, 480, 320);
static_assert(full.x == 0 && full.y == 0 && full.width == 320 && full.height == 480);
constexpr auto top = CompactLcdTestNativeWindow(0, 0, 480, 1);
static_assert(top.x == 319 && top.y == 0 && top.width == 1 && top.height == 480);
constexpr auto bottom = CompactLcdTestNativeWindow(0, 319, 480, 1);
static_assert(bottom.x == 0 && bottom.y == 0 && bottom.width == 1 && bottom.height == 480);
constexpr auto centered = CompactLcdTestNativeWindow(40, 0, 400, 1);
static_assert(centered.x == 319 && centered.y == 40 && centered.height == 400);
constexpr auto native = CompactLcdTestNativeWindow(160, 96, 160, 128);
static_assert(native.x == 96 && native.y == 160 && native.width == 128 && native.height == 160);
static_assert(CompactLcdTestNativeWindow(0, 0, 481, 1).width == 0);
static_assert(CompactLcdTestNativeWindow(0, 320, 480, 1).height == 0);
