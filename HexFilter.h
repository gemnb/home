#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HexSearchCondition {
    int position = 0;
    uint8_t value = 0;
    bool isWildcard = false;

    HexSearchCondition() = default;
    HexSearchCondition(int pos, uint8_t val, bool wildcard = false)
        : position(pos), value(val), isWildcard(wildcard) {
    }
};

class HexFilter {
public:
    static std::vector<HexSearchCondition> ParsePattern(const std::string& pattern);
    static bool MatchPattern(const uint8_t* data, size_t dataLen, const std::vector<HexSearchCondition>& conditions);
    static bool ParseHexByte(const std::string& hex, uint8_t& result, bool& isWildcard);
    static std::string BytesToHexString(const uint8_t* data, size_t len);
};
