#pragma once

#include <string_view>
#include "../../device_state.h"

constexpr std::string_view ResolveCompactTftEmotion(DeviceState state, std::string_view requested) {
    if (state == kDeviceStateIdle || state == kDeviceStateUnknown)
        return "neutral";
    if (state == kDeviceStateListening)
        return "attentive";
    if (state == kDeviceStateConnecting)
        return "thinking";
    const bool neutral =
        requested.empty() || requested == "neutral" || requested == "normal" || requested == "idle";
    if (state == kDeviceStateSpeaking && neutral)
        return "chatty";
    return neutral ? std::string_view("neutral") : requested;
}
