#include "DisconnectRuleEngine.h"
#include "ABProtectSDK.h"

#include <algorithm>

namespace {
void ClearHexBuffers(DisconnectRuntimeState& state) {
    state.hexClientStreamBuf.clear();
    state.hexServerStreamBuf.clear();
}
}

void DisconnectRuleEngine::ResetRuntimeState(DisconnectRuntimeState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.forwardingStartTime = std::chrono::steady_clock::now();
    state.hexMatchTime = std::chrono::steady_clock::time_point{};
    state.hexMatched = false;
    state.hexStreamDone = false;
    ClearHexBuffers(state);
    state.disconnectRequested.store(false);
}

bool DisconnectRuleEngine::HasEnabledRules(const std::vector<DisconnectRule>& rules) {
    return std::any_of(rules.begin(), rules.end(), [](const DisconnectRule& rule) {
        return rule.enabled;
    });
}

const DisconnectRule* DisconnectRuleEngine::FindMatchingRule(const std::vector<DisconnectRule>& rules, uint16_t targetPort) {
    for (const auto& rule : rules) {
        if (rule.enabled && rule.targetPort == targetPort) {
            return &rule;
        }
    }

    return nullptr;
}

DisconnectFeedResult DisconnectRuleEngine::FeedData(
    const std::vector<DisconnectRule>& rules,
    uint16_t targetPort,
    DisconnectRuntimeState& state,
    const std::vector<uint8_t>& data,
    DisconnectDirection direction) {
    ABPROTECT_CFF_BEGIN;
    ABPROTECT_CHECK_DEBUGGER;
    DisconnectFeedResult result;

    const DisconnectRule* matchedRule = FindMatchingRule(rules, targetPort);
    if (!matchedRule || data.empty()) {
        return result;
    }

    if (matchedRule->mode != DisconnectRuleMode::Hex) {
        return result;
    }

    if (matchedRule->cachedConditions.empty() || matchedRule->cachedMaxPosition < 0) {
        return result;
    }

    const size_t needBytes = static_cast<size_t>(matchedRule->cachedMaxPosition) + 1;
    std::lock_guard<std::mutex> lock(state.mutex);

    if (state.hexStreamDone) {
        return result;
    }

    auto& streamBuf = (direction == DisconnectDirection::ClientToServer)
        ? state.hexClientStreamBuf
        : state.hexServerStreamBuf;

    if (streamBuf.size() < needBytes) {
        const size_t canAppend = needBytes - streamBuf.size();
        const size_t appendCount = (std::min)(canAppend, data.size());
        streamBuf.insert(streamBuf.end(), data.begin(), data.begin() + appendCount);
    }

    const auto tryMatch = [&](const std::vector<uint8_t>& buffer) -> bool {
        if (buffer.empty()) {
            return false;
        }
        return HexFilter::MatchPattern(buffer.data(), buffer.size(), matchedRule->cachedConditions);
    };

    if (tryMatch(state.hexClientStreamBuf) || tryMatch(state.hexServerStreamBuf)) {
        result.matched = true;
        state.hexStreamDone = true;
        if (matchedRule->hexDelayEnabled && matchedRule->hexDelaySeconds > 0) {
            if (!state.hexMatched) {
                state.hexMatched = true;
                state.hexMatchTime = std::chrono::steady_clock::now();
                result.delayStarted = true;
            }
        }
        else {
            result.disconnectNow = true;
        }
        ClearHexBuffers(state);
        return result;
    }

    const bool clientDone = state.hexClientStreamBuf.size() >= needBytes;
    const bool serverDone = state.hexServerStreamBuf.size() >= needBytes;
    if (clientDone && serverDone) {
        state.hexStreamDone = true;
        ClearHexBuffers(state);
    }

    ABPROTECT_CFF_END;
    return result;
}

bool DisconnectRuleEngine::ShouldDisconnectNow(
    const std::vector<DisconnectRule>& rules,
    uint16_t targetPort,
    DisconnectRuntimeState& state,
    std::string* reason) {
    const DisconnectRule* matchedRule = FindMatchingRule(rules, targetPort);
    if (!matchedRule) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    const auto now = std::chrono::steady_clock::now();

    if (matchedRule->mode == DisconnectRuleMode::Timer && matchedRule->disconnectIntervalSec > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - state.forwardingStartTime).count();
        if (elapsed >= static_cast<int64_t>(matchedRule->disconnectIntervalSec)) {
            if (reason) {
                *reason = "达到定时断开时间";
            }
            return true;
        }
    } else if (matchedRule->mode == DisconnectRuleMode::Hex && state.hexMatched) {
        if (matchedRule->hexDelayEnabled && matchedRule->hexDelaySeconds > 0) {
            const auto hexElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - state.hexMatchTime).count();
            if (hexElapsed >= static_cast<int64_t>(matchedRule->hexDelaySeconds)) {
                if (reason) {
                    *reason = "命中十六进制规则延迟断开";
                }
                return true;
            }
        }
    }

    return false;
}
