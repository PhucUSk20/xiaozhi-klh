#pragma once

// Wire encoding is independent of LVGL and the CPU's byte order.
constexpr unsigned CompactLcdTestPixel(unsigned char* out, unsigned r, unsigned g, unsigned b,
                                       bool rgb666) {
    if (rgb666) {
        out[0] = r & 0xFC;
        out[1] = g & 0xFC;
        out[2] = b & 0xFC;
        return 3;
    }
    const unsigned pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    out[0] = pixel >> 8;
    out[1] = pixel & 0xFF;
    return 2;
}

struct CompactLcdTestSize {
    unsigned width;
    unsigned height;
};

struct CompactLcdTestWindow {
    unsigned x, y, width, height;
};

// Rotate logical 480x320 coordinates into the controller's native 320x480
// address space. Avoid relying on MADCTL MV for full-width landscape writes.
constexpr CompactLcdTestWindow CompactLcdTestNativeWindow(unsigned x, unsigned y, unsigned width,
                                                          unsigned height) {
    if (!width || !height || x >= 480 || y >= 320 || width > 480 - x || height > 320 - y)
        return {0, 0, 0, 0};
    return {320 - y - height, x, height, width};
}

constexpr CompactLcdTestSize CompactLcdTestFit(unsigned width, unsigned height,
                                               unsigned screen_width, unsigned screen_height) {
    if (!width || !height || !screen_width || !screen_height)
        return {0, 0};
    if (screen_width * height <= screen_height * width)
        return {screen_width, height * screen_width / width};
    return {width * screen_height / height, screen_height};
}
