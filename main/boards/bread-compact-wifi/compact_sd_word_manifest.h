#pragma once

#include <cstddef>
#include <string_view>

constexpr std::size_t kCompactSdManifestMaxBytes = 2 * 1024 * 1024;
constexpr int kCompactSdManifestMaxEntries = 4000;

constexpr bool IsCompactSdManifestHeader(double version, double count, int actual_count) {
    return version == 1 && actual_count >= 1 && actual_count <= kCompactSdManifestMaxEntries &&
           count == actual_count;
}

constexpr bool IsCompactSdSha256(std::string_view text) {
    if (text.size() != 64)
        return false;
    for (char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

constexpr bool IsCompactSdWordMapping(std::string_view key, std::string_view lesson,
                                      std::string_view file, std::string_view sha) {
    return !lesson.empty() && lesson.size() <= 100 && key.size() <= 260 &&
           key.size() > lesson.size() + 1 && key.substr(0, lesson.size()) == lesson &&
           key[lesson.size()] == ':' && file.size() == 80 && file.substr(0, 12) == "word-images/" &&
           file.substr(76) == ".jpg" && IsCompactSdSha256(file.substr(12, 64)) &&
           IsCompactSdSha256(sha);
}
