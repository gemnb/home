#include "DisconnectRuleTypes.h"

#include "res/json.hpp"

#include <algorithm>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace {
std::string BuildRuleName(uint16_t targetPort) {
    return "端口 " + std::to_string(targetPort) + " 规则";
}
}

bool DisconnectRuleCodec::ValidateRule(const DisconnectRule& rule, std::string* error) {
    if (rule.targetPort == 0 || rule.targetPort > 65535) {
        if (error) {
            *error = BuildRuleName(rule.targetPort) + " 的端口无效";
        }
        return false;
    }

    if (rule.mode == DisconnectRuleMode::Timer) {
        if (rule.disconnectIntervalSec == 0) {
            if (error) {
                *error = BuildRuleName(rule.targetPort) + " 的定时断开秒数必须大于 0";
            }
            return false;
        }
    } else if (rule.mode == DisconnectRuleMode::Hex) {
        if (rule.hexPattern.empty()) {
            if (error) {
                *error = BuildRuleName(rule.targetPort) + " 的 Hex 规则不能为空";
            }
            return false;
        }

        const auto conditions = HexFilter::ParsePattern(rule.hexPattern);
        if (conditions.empty()) {
            if (error) {
                *error = BuildRuleName(rule.targetPort) + " 的十六进制规则格式无效";
            }
            return false;
        }
    } else {
        if (error) {
            *error = BuildRuleName(rule.targetPort) + " 的断网模式无效";
        }
        return false;
    }

    return true;
}

bool DisconnectRuleCodec::PrepareRules(const std::vector<DisconnectRule>& inputRules, std::vector<DisconnectRule>& preparedRules, std::string* error) {
    preparedRules.clear();

    std::set<uint16_t> seenPorts;
    preparedRules.reserve(inputRules.size());

    for (const auto& inputRule : inputRules) {
        if (!ValidateRule(inputRule, error)) {
            return false;
        }

        if (!seenPorts.insert(inputRule.targetPort).second) {
            if (error) {
                *error = "检测到重复的断网端口规则: " + std::to_string(inputRule.targetPort);
            }
            return false;
        }

        DisconnectRule prepared = inputRule;
        if (prepared.mode == DisconnectRuleMode::Timer) {
            prepared.hexPattern.clear();
            prepared.hexDelayEnabled = false;
            prepared.hexDelaySeconds = 0;
        } else {
            prepared.disconnectIntervalSec = 0;
        }
        prepared.cachedConditions.clear();
        prepared.cachedMaxPosition = -1;

        if (prepared.mode == DisconnectRuleMode::Hex && !prepared.hexPattern.empty()) {
            prepared.cachedConditions = HexFilter::ParsePattern(prepared.hexPattern);
            for (const auto& condition : prepared.cachedConditions) {
                if (condition.position > prepared.cachedMaxPosition) {
                    prepared.cachedMaxPosition = condition.position;
                }
            }
        }

        preparedRules.push_back(std::move(prepared));
    }

    return true;
}

bool DisconnectRuleCodec::DeserializeRulesFromJson(const std::string& jsonText, std::vector<DisconnectRule>& rules, std::string* error) {
    rules.clear();

    if (jsonText.empty()) {
        return true;
    }

    json parsed = json::parse(jsonText, nullptr, false);
    if (parsed.is_discarded()) {
        if (error) {
            *error = "断网规则 JSON 解析失败";
        }
        return false;
    }

    if (!parsed.is_array()) {
        if (error) {
            *error = "断网规则必须是数组";
        }
        return false;
    }

    std::vector<DisconnectRule> loadedRules;
    loadedRules.reserve(parsed.size());

    for (const auto& item : parsed) {
        DisconnectRule rule;
        rule.targetPort = static_cast<uint16_t>(item.value("targetPort", 0));
        if (item.contains("mode")) {
            rule.mode = static_cast<DisconnectRuleMode>(item.value("mode", 0));
        } else {
            rule.mode = item.value("hexPattern", std::string()).empty()
                ? DisconnectRuleMode::Timer
                : DisconnectRuleMode::Hex;
        }
        rule.disconnectIntervalSec = static_cast<uint32_t>(item.value("disconnectIntervalSec", 0));
        rule.enabled = item.value("enabled", true);
        rule.hexPattern = item.value("hexPattern", "");
        rule.hexDelayEnabled = item.value("hexDelayEnabled", false);
        rule.hexDelaySeconds = static_cast<uint32_t>(item.value("hexDelaySeconds", 0));
        loadedRules.push_back(std::move(rule));
    }

    std::vector<DisconnectRule> preparedRules;
    if (!PrepareRules(loadedRules, preparedRules, error)) {
        return false;
    }

    rules = std::move(preparedRules);
    return true;
}

std::string DisconnectRuleCodec::SerializeRulesToJson(const std::vector<DisconnectRule>& rules) {
    json output = json::array();
    for (const auto& rule : rules) {
        json item;
        item["targetPort"] = rule.targetPort;
        item["mode"] = static_cast<int>(rule.mode);
        item["disconnectIntervalSec"] = rule.disconnectIntervalSec;
        item["enabled"] = rule.enabled;
        item["hexPattern"] = rule.hexPattern;
        item["hexDelayEnabled"] = rule.hexDelayEnabled;
        item["hexDelaySeconds"] = rule.hexDelaySeconds;
        output.push_back(std::move(item));
    }
    return output.dump();
}
