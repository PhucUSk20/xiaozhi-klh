#include "../../main/boards/bread-compact-wifi/compact_sd_gif_validation.h"

static_assert(IsCompactSdGifDirectory("Kira-Emotes 2/Kira-Emotes/gif",
                                      sizeof("Kira-Emotes 2/Kira-Emotes/gif") - 1));
static_assert(!IsCompactSdGifDirectory("../gifs", 7));
static_assert(!IsCompactSdGifDirectory("gifs/../other", 13));
static_assert(!IsCompactSdGifDirectory("F:/gifs", 7));
static_assert(!IsCompactSdGifDirectory("/gifs", 5));
static_assert(!IsCompactSdGifDirectory("gifs/", 5));
static_assert(!IsCompactSdGifDirectory("gifs//more", 10));
static_assert(IsCompactSdGifDirectory("gifs\\more", 9));
static_assert(IsCompactSdGifDirectory("gifs\\more/mixed", sizeof("gifs\\more/mixed") - 1));
static_assert(!IsCompactSdGifDirectory("\\gifs", 5));
static_assert(!IsCompactSdGifDirectory("gifs\\", 5));
static_assert(!IsCompactSdGifDirectory("gifs\\..\\other", sizeof("gifs\\..\\other") - 1));
static_assert(!IsCompactSdGifDirectory("gifs/..\\other", sizeof("gifs/..\\other") - 1));
static_assert(!IsCompactSdGifDirectory("gifs\\/more", sizeof("gifs\\/more") - 1));
static_assert(!IsCompactSdGifDirectory("F:\\gifs", sizeof("F:\\gifs") - 1));

constexpr auto normalized = NormalizeCompactSdGifPath("gifs\\more/mixed");
static_assert(normalized.value[4] == '/' && normalized.value[9] == '/');
static_assert(sizeof(normalized.value) == sizeof("gifs/more/mixed"));
constexpr auto directory = NormalizeCompactSdGifPath("/sdcard/Kira-Emotes 3\\Kira-Emotes\\gif");
constexpr auto prefix = NormalizeCompactSdGifPath("Kira-Emotes 3\\Kira-Emotes\\gif/");
constexpr bool CheckNormalizedPaths() {
    constexpr char expected[] = "/sdcard/Kira-Emotes 3/Kira-Emotes/gif";
    for (decltype(sizeof(0)) i = 0; i < sizeof(expected); ++i)
        if (directory.value[i] != expected[i])
            return false;
    // The LVGL prefix must be exactly the mounted directory without /sdcard/,
    // followed by one slash; preserve spaces, dashes and the terminating null.
    for (decltype(sizeof(0)) i = 0; i < sizeof(prefix.value) - 2; ++i)
        if (prefix.value[i] != directory.value[i + 8])
            return false;
    return prefix.value[sizeof(prefix.value) - 2] == '/' &&
           prefix.value[sizeof(prefix.value) - 1] == 0;
}
static_assert(CheckNormalizedPaths(), "SD filesystem and LVGL paths must normalize identically");

static_assert(IsCompactSdGifName("happy.gif", 9));
static_assert(IsCompactSdGifName("Thinking.GIF", 12));
static_assert(!IsCompactSdGifName("../happy.gif", 12));
static_assert(!IsCompactSdGifName("gifs/happy.gif", 14));
static_assert(!IsCompactSdGifName("happy.png", 9));
static_assert(!IsCompactSdGifName(".gif", 4));

constexpr bool CheckGifHeader() {
    unsigned width = 0, height = 0;
    unsigned char header[13] = {'G', 'I', 'F', '8', '9', 'a', 0xE0, 1, 0x68, 1};
    if (!ParseCompactSdGifHeader(header, 13, width, height) || width != 480 || height != 360) {
        return false;
    }
    for (unsigned size = 0; size < 13; ++size) {
        if (ParseCompactSdGifHeader(header, size, width, height)) {
            return false;
        }
    }
    header[7] = 3;
    return !ParseCompactSdGifHeader(header, 13, width, height);
}
static_assert(CheckGifHeader());

struct GifBytes {
    const unsigned char* bytes;
    decltype(sizeof(0)) size;
    decltype(sizeof(0)) pos = 0;
    constexpr bool Read(unsigned char* out, decltype(sizeof(0)) count) {
        if (count > size - pos)
            return false;
        for (decltype(count) i = 0; i < count; ++i)
            out[i] = bytes[pos++];
        return true;
    }
    constexpr bool Skip(decltype(sizeof(0)) count) {
        if (count > size - pos)
            return false;
        pos += count;
        return true;
    }
    constexpr auto Remaining() const { return size - pos; }
};

constexpr bool CheckGifStream() {
    unsigned char gif[] = {'G', 'I', 'F', '8', '9', 'a', 1,   0,    1, 0, 0x80, 0,
                           0,   0,   0,   0,   255, 255, 255, 0x2C, 0, 0, 0,    0,
                           1,   0,   1,   0,   0,   2,   2,   0x44, 1, 0, 0x3B};
    unsigned w = 0, h = 0;
    GifBytes valid{gif, sizeof(gif)};
    if (!ValidateCompactSdGifStream(valid, w, h))
        return false;
    for (unsigned size = 0; size < sizeof(gif); ++size) {
        GifBytes truncated{gif, size};
        if (ValidateCompactSdGifStream(truncated, w, h))
            return false;
    }
    gif[29] = 9;  // Decoder's LZW code size table only supports 2..8.
    GifBytes invalid_code{gif, sizeof(gif)};
    if (ValidateCompactSdGifStream(invalid_code, w, h))
        return false;
    gif[29] = 2;
    gif[24] = 2;  // Frame exceeds the 1x1 canvas.
    GifBytes invalid_frame{gif, sizeof(gif)};
    if (ValidateCompactSdGifStream(invalid_frame, w, h))
        return false;
    unsigned char extension[] = {'G', 'I', 'F', '8', '9', 'a',  1,    0,
                                 1,   0,   0,   0,   0,   0x21, 0xFE, 255};
    GifBytes truncated_extension{extension, sizeof(extension)};
    return !ValidateCompactSdGifStream(truncated_extension, w, h);
}
static_assert(CheckGifStream());
