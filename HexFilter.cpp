#include "HexFilter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

std::vector<HexSearchCondition> HexFilter::ParsePattern(const std::string& pattern) {
    std::vector<HexSearchCondition> conditions;
    if (pattern.empty()) {
        return conditions;
    }

    std::stringstream ss(pattern);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(),
            [](unsigned char ch) { return std::isspace(ch) != 0; }), token.end());

        if (token.empty()) {
            continue;
        }

        const size_t pipePos = token.find('|');
        if (pipePos == std::string::npos) {
            continue;
        }

        const std::string posStr = token.substr(0, pipePos);
        const std::string hexStr = token.substr(pipePos + 1);

        int position = 0;
        try {
            position = std::stoi(posStr);
            if (position < 0) {
                continue;
            }
        }
        catch (...) {
            continue;
        }

        uint8_t value = 0;
        bool isWildcard = false;
        if (ParseHexByte(hexStr, value, isWildcard)) {
            conditions.emplace_back(position, value, isWildcard);
        }
    }

    return conditions;
}

bool HexFilter::MatchPattern(const uint8_t* data, size_t dataLen, const std::vector<HexSearchCondition>& conditions) {
    if (!data || conditions.empty()) {
        return false;
    }

    for (const auto& cond : conditions) {
        if (cond.position < 0) {
            return false;
        }
        if (static_cast<size_t>(cond.position) >= dataLen) {
            return false;
        }
        if (!cond.isWildcard && data[cond.position] != cond.value) {
            return false;
        }
    }

    return true;
}

bool HexFilter::ParseHexByte(const std::string& hex, uint8_t& result, bool& isWildcard) {
    if (hex.length() != 2) {
        return false;
    }

    if (hex == "??" || hex == "**") {
        isWildcard = true;
        result = 0;
        return true;
    }

    isWildcard = false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(hex.c_str(), &end, 16);
    if (end != hex.c_str() + 2 || value > 0xFF) {
        return false;
    }

    result = static_cast<uint8_t>(value);
    return true;
}

std::string HexFilter::BytesToHexString(const uint8_t* data, size_t len) {
    std::stringstream ss;
    ss << std::hex << std::uppercase;
    for (size_t index = 0; index < len; ++index) {
        if (index > 0) {
            ss << ' ';
        }
        ss.width(2);
        ss.fill('0');
        ss << static_cast<int>(data[index]);
    }
    return ss.str();
}
