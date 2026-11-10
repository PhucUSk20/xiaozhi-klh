#pragma once

constexpr bool IsCompactSdPathSeparator(char c) { return c == '/' || c == '\\'; }

template <decltype(sizeof(0)) N>
struct CompactSdGifPath {
    char value[N]{};
    constexpr explicit CompactSdGifPath(const char (&path)[N]) {
        for (decltype(N) i = 0; i < N; ++i)
            value[i] = path[i] == '\\' ? '/' : path[i];
    }
};

template <decltype(sizeof(0)) N>
constexpr CompactSdGifPath<N> NormalizeCompactSdGifPath(const char (&path)[N]) {
    return CompactSdGifPath<N>(path);
}

constexpr bool IsCompactSdGifDirectory(const char* path, decltype(sizeof(0)) size) {
    if (size == 0 || size > 128 || IsCompactSdPathSeparator(path[0]) ||
        IsCompactSdPathSeparator(path[size - 1]))
        return false;
    decltype(sizeof(0)) component = 0;
    for (decltype(sizeof(0)) i = 0; i <= size; ++i) {
        if (i < size && (static_cast<unsigned char>(path[i]) < 32 || path[i] == ':'))
            return false;
        if (i == size || IsCompactSdPathSeparator(path[i])) {
            const auto length = i - component;
            if (length == 0 || (length == 1 && path[component] == '.') ||
                (length == 2 && path[component] == '.' && path[component + 1] == '.'))
                return false;
            component = i + 1;
        }
    }
    return true;
}

constexpr bool IsCompactSdGifName(const char* name, decltype(sizeof(0)) length) {
    if (length < 5 || length > 64 || name[length - 4] != '.' ||
        (name[length - 3] != 'g' && name[length - 3] != 'G') ||
        (name[length - 2] != 'i' && name[length - 2] != 'I') ||
        (name[length - 1] != 'f' && name[length - 1] != 'F')) {
        return false;
    }
    for (decltype(length) i = 0; i < length - 4; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

constexpr bool ParseCompactSdGifHeader(const unsigned char* data, decltype(sizeof(0)) size,
                                       unsigned& width, unsigned& height) {
    width = height = 0;
    if (size < 13 || data[0] != 'G' || data[1] != 'I' || data[2] != 'F' || data[3] != '8' ||
        (data[4] != '7' && data[4] != '9') || data[5] != 'a') {
        return false;
    }
    width = unsigned(data[6]) | (unsigned(data[7]) << 8);
    height = unsigned(data[8]) | (unsigned(data[9]) << 8);
    return width > 0 && height > 0 && width <= 480 && height <= 480;
}

template <typename Reader>
constexpr bool ValidateCompactSdGifStream(Reader& reader, unsigned& width, unsigned& height) {
    unsigned char header[13]{};
    if (!reader.Read(header, 13) || !ParseCompactSdGifHeader(header, 13, width, height)) {
        return false;
    }
    const bool global_palette = (header[10] & 0x80) != 0;
    if (global_palette && !reader.Skip(3u * (1u << ((header[10] & 7) + 1)))) {
        return false;
    }
    unsigned frames = 0;
    auto subblocks = [&](bool require_data) {
        unsigned blocks = 0;
        unsigned char length = 0;
        while (reader.Read(&length, 1)) {
            if (length == 0) {
                return !require_data || blocks > 0;
            }
            if (!reader.Skip(length)) {
                return false;
            }
            ++blocks;
        }
        return false;
    };
    unsigned char marker = 0;
    while (reader.Read(&marker, 1)) {
        if (marker == 0x3B) {
            return frames > 0 && reader.Remaining() == 0;
        }
        if (marker == 0x21) {
            unsigned char label = 0;
            if (!reader.Read(&label, 1)) {
                return false;
            }
            if (label == 0xF9) {
                unsigned char control[6]{};
                if (!reader.Read(control, 6) || control[0] != 4 || control[5] != 0 ||
                    ((control[1] >> 2) & 7) > 3) {
                    return false;
                }
            } else if (label == 0xFF) {
                unsigned char length = 0;
                if (!reader.Read(&length, 1) || length != 11 || !reader.Skip(11) ||
                    !subblocks(false)) {
                    return false;
                }
            } else if (label == 0xFE) {
                if (!subblocks(false)) {
                    return false;
                }
            } else {
                // The installed decoder does not safely support plain-text extensions.
                return false;
            }
        } else if (marker == 0x2C) {
            unsigned char frame[9]{};
            if (!reader.Read(frame, 9) || ++frames > 1024) {
                return false;
            }
            const unsigned x = unsigned(frame[0]) | (unsigned(frame[1]) << 8);
            const unsigned y = unsigned(frame[2]) | (unsigned(frame[3]) << 8);
            const unsigned w = unsigned(frame[4]) | (unsigned(frame[5]) << 8);
            const unsigned h = unsigned(frame[6]) | (unsigned(frame[7]) << 8);
            const bool local_palette = (frame[8] & 0x80) != 0;
            if (!w || !h || x + w > width || y + h > height ||
                (!global_palette && !local_palette)) {
                return false;
            }
            if (local_palette && !reader.Skip(3u * (1u << ((frame[8] & 7) + 1)))) {
                return false;
            }
            unsigned char code_size = 0;
            if (!reader.Read(&code_size, 1) || code_size < 2 || code_size > 8 || !subblocks(true)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return false;
}
