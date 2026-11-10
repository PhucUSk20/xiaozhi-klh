#include "../../main/boards/bread-compact-wifi/compact_sd_word_manifest.h"

constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kFile =
    "word-images/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.jpg";

static_assert(IsCompactSdManifestHeader(1, 3468, 3468));  // Full EduHub pack, formerly rejected.
static_assert(IsCompactSdManifestHeader(1, 4000, 4000));
static_assert(!IsCompactSdManifestHeader(1, 4001, 4001));
static_assert(!IsCompactSdManifestHeader(1, 3467, 3468));
static_assert(!IsCompactSdManifestHeader(1, 3468.5, 3468));
static_assert(!IsCompactSdManifestHeader(2, 3468, 3468));
static_assert(!IsCompactSdManifestHeader(1, 0, 0));
static_assert(IsCompactSdWordMapping("contracts:abide by", "contracts", kFile, kSha));
static_assert(!IsCompactSdWordMapping("other:abide by", "contracts", kFile, kSha));
static_assert(!IsCompactSdWordMapping("contracts:", "contracts", kFile, kSha));
static_assert(!IsCompactSdWordMapping("contracts:a", "", kFile, kSha));
static_assert(!IsCompactSdWordMapping("contracts:a", "contracts", "../word-images/test.jpg", kSha));
static_assert(!IsCompactSdWordMapping("contracts:a", "contracts", kFile, "invalid"));
static_assert(
    !IsCompactSdSha256("ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"));
