#pragma once

#include "DisconnectRuleTypes.h"

#include <string>
#include <vector>

struct DisconnectFeedResult {
    bool matched = false;
    bool disconnectNow = false;
    bool delayStarted = false;
};

class DisconnectRuleEngine {
public:
    static void ResetRuntimeState(DisconnectRuntimeState& state);
    static bool HasEnabledRules(const std::vector<DisconnectRule>& rules);
    static DisconnectFeedResult FeedData(
        const std::vector<DisconnectRule>& rules,
        uint16_t targetPort,
        DisconnectRuntimeState& state,
        const std::vector<uint8_t>& data,
        DisconnectDirection direction);
    static bool ShouldDisconnectNow(
        const std::vector<DisconnectRule>& rules,
        uint16_t targetPort,
        DisconnectRuntimeState& state,
        std::string* reason = nullptr);

private:
    static const DisconnectRule* FindMatchingRule(const std::vector<DisconnectRule>& rules, uint16_t targetPort);
};
