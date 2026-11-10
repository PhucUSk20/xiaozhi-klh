#include "../../main/boards/bread-compact-wifi/compact_caro_board.h"

static_assert(IsValidCompactCaroBoard("123456789"));
static_assert(IsValidCompactCaroBoard("O234X6789"));
static_assert(IsValidCompactCaroBoard("XOXOOXXXO"));
static_assert(!IsValidCompactCaroBoard(""));
static_assert(!IsValidCompactCaroBoard("end"));
static_assert(!IsValidCompactCaroBoard("12345678"));
static_assert(!IsValidCompactCaroBoard("1234567890"));
static_assert(!IsValidCompactCaroBoard("1234x6789"));
static_assert(!IsValidCompactCaroBoard("1234.6789"));
static_assert(!IsValidCompactCaroBoard("987654321"));
static_assert(!IsValidCompactCaroBoard("000000000"));
static_assert(
    !IsValidCompactCaroBoard(std::string_view("1234\0"
                                              "6789",
                                              9)));
