#pragma once

// Input is big-endian RGB565 (LVGL's swap_bytes output); RGB666 uses the
// upper six bits of each of three bytes. Replicate the top bit of 5-bit R/B
// when expanding to six bits so full intensity remains full intensity.
constexpr void CompactRgb565ToRgb666(unsigned char* out, unsigned char high, unsigned char low) {
    const unsigned pixel = (unsigned(high) << 8) | low;
    const unsigned red = (pixel >> 11) & 31;
    const unsigned green = (pixel >> 5) & 63;
    const unsigned blue = pixel & 31;
    out[0] = ((red << 1) | (red >> 4)) << 2;
    out[1] = green << 2;
    out[2] = ((blue << 1) | (blue >> 4)) << 2;
}
