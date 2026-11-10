#include "../../main/boards/bread-compact-wifi/compact_rgb565_to_rgb666.h"

constexpr bool CheckPrimaryColors() {
    unsigned char out[3]{};
    CompactRgb565ToRgb666(out, 0xF8, 0x00);
    if (out[0] != 0xFC || out[1] != 0 || out[2] != 0)
        return false;
    CompactRgb565ToRgb666(out, 0x07, 0xE0);
    if (out[0] != 0 || out[1] != 0xFC || out[2] != 0)
        return false;
    CompactRgb565ToRgb666(out, 0x00, 0x1F);
    if (out[0] != 0 || out[1] != 0 || out[2] != 0xFC)
        return false;
    CompactRgb565ToRgb666(out, 0xFF, 0xFF);
    return out[0] == 0xFC && out[1] == 0xFC && out[2] == 0xFC;
}

constexpr bool CheckRange(unsigned start) {
    for (unsigned pixel = start; pixel < start + 4096; ++pixel) {
        unsigned char out[3] = {0xFF, 0xFF, 0xFF};
        CompactRgb565ToRgb666(out, pixel >> 8, pixel & 0xFF);
        if ((out[0] & 3) || (out[1] & 3) || (out[2] & 3))
            return false;
        // Reduction back to RGB565 must preserve every original pixel.
        const unsigned restored = ((unsigned(out[0]) >> 3) << 11) | ((unsigned(out[1]) >> 2) << 5) |
                                  (unsigned(out[2]) >> 3);
        if (restored != pixel)
            return false;
    }
    return true;
}
static_assert(CheckPrimaryColors(), "RGB666 primary colors/byte order are incorrect");
static_assert(CheckRange(0x0000) && CheckRange(0x1000), "RGB666 range 0");
static_assert(CheckRange(0x2000) && CheckRange(0x3000), "RGB666 range 1");
static_assert(CheckRange(0x4000) && CheckRange(0x5000), "RGB666 range 2");
static_assert(CheckRange(0x6000) && CheckRange(0x7000), "RGB666 range 3");
static_assert(CheckRange(0x8000) && CheckRange(0x9000), "RGB666 range 4");
static_assert(CheckRange(0xA000) && CheckRange(0xB000), "RGB666 range 5");
static_assert(CheckRange(0xC000) && CheckRange(0xD000), "RGB666 range 6");
static_assert(CheckRange(0xE000) && CheckRange(0xF000), "RGB666 range 7");
