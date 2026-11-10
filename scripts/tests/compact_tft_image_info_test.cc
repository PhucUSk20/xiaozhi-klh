#include "../../main/boards/bread-compact-wifi/compact_tft_image_info.h"

constexpr bool CheckImageHeaders() {
    CompactTftImageInfo info;
    unsigned char png[33] = {137, 80,  78,  71,  13, 10, 26, 10,  0, 0, 0, 13,
                             'I', 'H', 'D', 'R', 0,  0,  0,  128, 0, 0, 0, 160};
    if (!ParseCompactTftImage(png, sizeof(png), info) || info.width != 128 || info.height != 160 ||
        info.jpeg) {
        return false;
    }
    for (unsigned size = 0; size < 33; ++size) {
        if (ParseCompactTftImage(png, size, info)) {
            return false;
        }
    }
    png[17] = 1;  // 65664 pixels wide must not truncate to 128.
    if (ParseCompactTftImage(png, sizeof(png), info)) {
        return false;
    }
    png[17] = 0;
    png[18] = 1;
    png[19] = 0xE0;  // 480x320 fits the ILI9486 profile only.
    png[22] = 1;
    png[23] = 0x40;
    if (ParseCompactTftImage(png, sizeof(png), info) ||
        !ParseCompactTftImage(png, sizeof(png), info, 480) || info.width != 480 ||
        info.height != 320) {
        return false;
    }
    png[19] = 0xE1;  // 481 pixels still exceeds the large panel's limit.
    if (ParseCompactTftImage(png, sizeof(png), info, 480)) {
        return false;
    }
    unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xC0, 0, 17,   8, 0, 160,  0, 128,
                            3,    1,    0x22, 0,    2, 0x11, 1, 3, 0x11, 1};
    if (!ParseCompactTftImage(jpeg, sizeof(jpeg), info) || !info.jpeg || info.width != 128 ||
        info.height != 160) {
        return false;
    }
    for (unsigned size = 0; size < sizeof(jpeg); ++size) {
        if (ParseCompactTftImage(jpeg, size, info)) {
            return false;
        }
    }
    jpeg[3] = 0xC2;  // Progressive JPEG is unsupported.
    if (ParseCompactTftImage(jpeg, sizeof(jpeg), info)) {
        return false;
    }
    jpeg[3] = 0xC0;
    jpeg[7] = 1;
    jpeg[8] = 0x40;
    jpeg[9] = 1;
    jpeg[10] = 0xE0;
    if (ParseCompactTftImage(jpeg, sizeof(jpeg), info) ||
        !ParseCompactTftImage(jpeg, sizeof(jpeg), info, 480) || info.width != 480 ||
        info.height != 320) {
        return false;
    }
    jpeg[4] = 0xFF;  // Segment length exceeds available input.
    return !ParseCompactTftImage(jpeg, sizeof(jpeg), info);
}

static_assert(CheckImageHeaders(), "TFT image header validation failed");
