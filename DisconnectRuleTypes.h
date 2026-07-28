#pragma once

#include "HexFilter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

enum class DisconnectDirection {
    ClientToServer = 0,
    ServerToClient = 1,
};

enum class DisconnectRuleMode {
    Timer = 0,
    Hex = 1,
};

struct DisconnectRule {
    uint16_t targetPort = 0;
    DisconnectRuleMode mode = DisconnectRuleMode::Timer;
    uint32_t disconnectIntervalSec = 0;
    bool enabled = true;
    std::string hexPattern;
    bool hexDelayEnabled = false;
    uint32_t hexDelaySeconds = 0;

    std::vector<HexSearchCondition> cachedConditions;
    int cachedMaxPosition = -1;
};

struct DisconnectRuntimeState {
    std::chrono::steady_clock::time_point forwardingStartTime{};
    std::chrono::steady_clock::time_point hexMatchTime{};
    bool hexMatched = false;
    bool hexStreamDone = false;
    std::vector<uint8_t> hexClientStreamBuf;
    std::vector<uint8_t> hexServerStreamBuf;
    std::mutex mutex;
    std::atomic<bool> disconnectRequested{ false };

    DisconnectRuntimeState() = default;
    DisconnectRuntimeState(const DisconnectRuntimeState&) = delete;
    DisconnectRuntimeState& operator=(const DisconnectRuntimeState&) = delete;
};

namespace DisconnectRuleCodec {
    bool ValidateRule(const DisconnectRule& rule, std::string* error = nullptr);
    bool PrepareRules(const std::vector<DisconnectRule>& inputRules, std::vector<DisconnectRule>& preparedRules, std::string* error = nullptr);
    bool DeserializeRulesFromJson(const std::string& jsonText, std::vector<DisconnectRule>& rules, std::string* error = nullptr);
    std::string SerializeRulesToJson(const std::vector<DisconnectRule>& rules);
}
