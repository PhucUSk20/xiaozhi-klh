#include "../../main/boards/bread-compact-wifi/compact_tft_gif_raster.h"

constexpr bool CheckRasterRows() {
    // 2x2 BGRA: red/green in row 0; blue/transparent white in row 1.
    const unsigned char source[] = {0,   0, 255, 255, 0,   255, 0,   255,
                                    255, 0, 0,   255, 255, 255, 255, 0};
    unsigned char destination[52]{};
    destination[0] = 0xA1;
    destination[51] = 0xB2;
    if (!CompactTftGifRaster(source, sizeof(source), 2, 2, destination + 1, 50, 5, 5))
        return false;
    for (unsigned y = 0; y < 5; ++y) {
        for (unsigned x = 0; x < 5; ++x) {
            const unsigned expected = y < 3 ? (x < 3 ? 0xF800 : 0x07E0) : (x < 3 ? 0x001F : 0x0000);
            const unsigned offset = 1 + (y * 5 + x) * 2;
            if (destination[offset] != (expected & 0xFF) ||
                destination[offset + 1] != (expected >> 8))
                return false;
        }
    }
    if (destination[0] != 0xA1 || destination[51] != 0xB2)
        return false;
    if (CompactTftGifRaster(source, 15, 2, 2, destination + 1, 50, 5, 5) ||
        CompactTftGifRaster(source, 16, 2, 2, destination + 1, 49, 5, 5))
        return false;
    const unsigned char alpha[] = {255, 255, 255, 128};
    unsigned char pixel[2]{};
    return CompactTftGifRaster(alpha, 4, 1, 1, pixel, 2, 1, 1) && pixel[0] == 0x10 &&
           pixel[1] == 0x84;
}

constexpr auto fit = CompactTftGifFit(160, 128, 480, 320);
constexpr auto thin = CompactTftGifFit(1, 480, 480, 320);
static_assert(thin.width == 1 && thin.height == 320,
              "Thin GIF must not produce a zero-width raster");
static_assert(fit.width == 400 && fit.height == 320, "GIF must fit with preserved aspect ratio");
static_assert(CheckRasterRows(), "Raster rows/stride, alpha, byte order or bounds are incorrect");
