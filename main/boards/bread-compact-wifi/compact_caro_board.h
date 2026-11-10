#pragma once

#include <string_view>

// Squares 1..9 run from top-left to bottom-right. Empty squares retain their own digit.
constexpr bool IsValidCompactCaroBoard(std::string_view cells) {
    if (cells.size() != 9) {
        return false;
    }
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (cells[i] != 'X' && cells[i] != 'O' && cells[i] != static_cast<char>('1' + i)) {
            return false;
        }
    }
    return true;
}
