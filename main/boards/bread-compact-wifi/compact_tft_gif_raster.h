#pragma once

struct CompactTftGifSize {
    unsigned width;
    unsigned height;
};

// Same fit calculation as the working standalone LCD diagnostic.
constexpr CompactTftGifSize CompactTftGifFit(unsigned width, unsigned height, unsigned screen_width,
                                             unsigned screen_height) {
    if (!width || !height || !screen_width || !screen_height || width > 480 || height > 480 ||
        screen_width > 480 || screen_height > 480)
        return {0, 0};
    if (screen_width * height <= screen_height * width) {
        const unsigned fitted_height = height * screen_width / width;
        return {screen_width, fitted_height ? fitted_height : 1};
    }
    const unsigned fitted_width = width * screen_height / height;
    return {fitted_width ? fitted_width : 1, screen_height};
}

// Compose on black, sample nearest-neighbor, and write tightly packed native
// little-endian RGB565. LVGL displays this at 1:1; its flush swaps bytes later.
constexpr bool CompactTftGifRaster(const unsigned char* bgra, decltype(sizeof(0)) source_size,
                                   unsigned source_width, unsigned source_height,
                                   unsigned char* rgb565, decltype(sizeof(0)) output_size,
                                   unsigned width, unsigned height) {
    if (!bgra || !rgb565 || !source_width || !source_height || !width || !height ||
        source_width > 480 || source_height > 480 || width > 480 || height > 480 ||
        source_size < source_width * source_height * 4u || output_size < width * height * 2u)
        return false;
    for (unsigned y = 0; y < height; ++y) {
        const unsigned source_y = y * source_height / height;
        for (unsigned x = 0; x < width; ++x) {
            const unsigned source_x = x * source_width / width;
            const auto* pixel = bgra + (source_y * source_width + source_x) * 4;
            const unsigned alpha = pixel[3];
            const unsigned r = pixel[2] * alpha / 255;
            const unsigned g = pixel[1] * alpha / 255;
            const unsigned b = pixel[0] * alpha / 255;
            const unsigned value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            const unsigned offset = (y * width + x) * 2;
            rgb565[offset] = value & 0xFF;
            rgb565[offset + 1] = value >> 8;
        }
    }
    return true;
}
