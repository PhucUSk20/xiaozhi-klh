#pragma once

// Independent of ESP-IDF so malformed header cases can be checked on the host.
struct CompactTftImageInfo {
    unsigned width = 0;
    unsigned height = 0;
    bool jpeg = false;
};

constexpr bool ParseCompactTftImage(const unsigned char* data, decltype(sizeof(0)) size,
                                    CompactTftImageInfo& info, unsigned max_dimension = 320) {
    info = {};
    constexpr unsigned char png_signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    bool png = size >= 33;
    for (unsigned i = 0; png && i < 8; ++i) {
        png = data[i] == png_signature[i];
    }
    if (png) {
        if (data[8] != 0 || data[9] != 0 || data[10] != 0 || data[11] != 13 || data[12] != 'I' ||
            data[13] != 'H' || data[14] != 'D' || data[15] != 'R') {
            return false;
        }
        info.width = (unsigned(data[16]) << 24) | (unsigned(data[17]) << 16) |
                     (unsigned(data[18]) << 8) | data[19];
        info.height = (unsigned(data[20]) << 24) | (unsigned(data[21]) << 16) |
                      (unsigned(data[22]) << 8) | data[23];
    } else {
        if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
            return false;
        }
        info.jpeg = true;
        decltype(size) pos = 2;
        while (pos < size) {
            if (data[pos++] != 0xFF) {
                return false;
            }
            while (pos < size && data[pos] == 0xFF) {
                ++pos;
            }
            if (pos >= size) {
                return false;
            }
            const unsigned marker = data[pos++];
            if (marker == 0xDA || marker == 0xD9 || size - pos < 2) {
                return false;
            }
            const unsigned length = (unsigned(data[pos]) << 8) | data[pos + 1];
            if (length < 2 || length > size - pos) {
                return false;
            }
            if (marker == 0xC0) {
                if (length < 11 || data[pos + 2] != 8 ||
                    (data[pos + 7] != 1 && data[pos + 7] != 3) ||
                    length != 8u + 3u * data[pos + 7]) {
                    return false;
                }
                info.height = (unsigned(data[pos + 3]) << 8) | data[pos + 4];
                info.width = (unsigned(data[pos + 5]) << 8) | data[pos + 6];
                break;
            }
            pos += length;
        }
    }
    return info.width > 0 && info.height > 0 && info.width <= max_dimension &&
           info.height <= max_dimension;
}
