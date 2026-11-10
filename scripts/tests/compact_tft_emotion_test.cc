#include "../../main/boards/bread-compact-wifi/compact_tft_emotion.h"

static_assert(ResolveCompactTftEmotion(kDeviceStateIdle, "happy") == "neutral");
static_assert(ResolveCompactTftEmotion(kDeviceStateListening, "neutral") == "attentive");
static_assert(ResolveCompactTftEmotion(kDeviceStateListening, "happy") == "attentive");
static_assert(ResolveCompactTftEmotion(kDeviceStateSpeaking, "neutral") == "chatty");
static_assert(ResolveCompactTftEmotion(kDeviceStateSpeaking, "happy") == "happy");
static_assert(ResolveCompactTftEmotion(kDeviceStateSpeaking, "thinking") == "thinking");
static_assert(ResolveCompactTftEmotion(kDeviceStateConnecting, "neutral") == "thinking");
