// HeartbeatInstanceUI.inl
// 伪心跳实例UI界面
// 从单伪项目迁移的完整UI

#pragma once

#include "imgui.h"
#include "InstanceManager.h"
#include "HeartbeatCore.h"
#include "SingleInstanceInterop.h"
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <iomanip>

// ==================== UI状态结构 ====================
#if 0
inline void RenderHeartbeatInstanceConfig(HeartbeatInstance* instance, bool& needsRedraw) {
    if (!instance) {
        ImGuiGBK::Text("实例不存在");
        return;
    }

    SingleInstanceInterop::RenderHeartbeatInstanceUi(instance, needsRedraw);
}
#endif

#if 1
struct HeartbeatInstanceUIState {
    // 当前选中的实例
    std::string selectedInstanceId;

    // 23偏移规则编辑状态
    bool showPattern23RuleEditor = false;
    bool isEditingPattern23Rule = false;
    int editingPattern23RuleId = -1;
    char pattern23RuleNameBuf[128] = "";
    int pattern23RuleOffset = 0;
    int pattern23RuleLength = 12;
    bool pattern23RuleEnabled = true;

    // 09偏移规则编辑状态
    bool showPattern09RuleEditor = false;
    bool isEditingPattern09Rule = false;
    int editingPattern09RuleId = -1;
    char pattern09RuleNameBuf[128] = "";
    int pattern09RuleOffset = 0;
    int pattern09RuleLength = 12;
    bool pattern09RuleEnabled = true;

    // 62偏移规则编辑状态
    bool showPattern62RuleEditor = false;
    bool isEditingPattern62Rule = false;
    int editingPattern62RuleId = -1;
    char pattern62RuleNameBuf[128] = "";
    int pattern62RuleOffset = 0;
    int pattern62RuleLength = 12;
    bool pattern62RuleEnabled = true;

    // 白名单规则编辑状态
    bool showWhitelistRuleEditor = false;
    bool isEditingWhitelistRule = false;
    int editingWhitelistRuleId = -1;
    char whitelistRuleNameBuf[128] = "";
    char whitelistRulePatternBuf[512] = "";
    bool whitelistRuleUsePatternSearch = true;
    bool whitelistRuleEnabled = true;

    // 黑名单规则编辑状态
    bool showBlacklistRuleEditor = false;
    bool isEditingBlacklistRule = false;
    int editingBlacklistRuleId = -1;
    char blacklistRuleNameBuf[128] = "";
    char blacklistRulePatternBuf[512] = "";
    bool blacklistRuleUsePatternSearch = true;
    bool blacklistRuleEnabled = true;

    // 替换数量规则编辑状态
    bool showReplaceCountRuleEditor = false;
    bool isEditingReplaceCountRule = false;
    int editingReplaceCountRuleId = -1;
    char replaceCountRuleNameBuf[128] = "";
    int replaceCountRuleLimit = 1;
    int replaceCountRulePostStrategy = 0;
    bool replaceCountRuleClearPool = false;
    bool replaceCountRuleApplyWpe = true;
    bool replaceCountRuleResetOnDisconnect = true;
    int replaceCountRulePostFakeN = 1;
    int replaceCountRulePostOriginalN = 1;
    bool replaceCountRuleEnabled = true;

    // 固定包选择状态
    bool showFixedPacketSelector = false;
    int fixedPacketSelectLength = 12;

    // 配置文件路径
    char configFilePath[512] = "";
};

// 每实例独立UI状态（满足“每个单伪实例独立 UI + 独立保存”）
static std::map<std::string, HeartbeatInstanceUIState> g_heartbeatInstUiStates;
static HeartbeatInstanceUIState& HbInstUI_GetState(const std::string& instanceId) {
    return g_heartbeatInstUiStates[instanceId];
}

// 兼容既有实现：下方大量代码使用 `g_heartbeatUIState.xxx`，这里用宏把它映射到“当前实例”的状态。
static HeartbeatInstanceUIState* g_heartbeatUIStatePtr = nullptr;
#define g_heartbeatUIState (*g_heartbeatUIStatePtr)

// ==================== 辅助函数 ====================
static std::string BytesToHexString(const std::vector<uint8_t>& data, int maxBytes = 32) {
    std::stringstream ss;
    int count = std::min(static_cast<int>(data.size()), maxBytes);
    for (int i = 0; i < count; i++) {
        ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(data[i]);
        if (i < count - 1) ss << " ";
    }
    if (static_cast<int>(data.size()) > maxBytes) {
        ss << " ...";
    }
    return ss.str();
}

// ==================== 基础配置UI ====================
static void RenderHeartbeatBasicConfig(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("基础配置");
    ImGui::Separator();

    // Pattern23模式选择
    ImGui::Text("23特征替换模式:");
    int pattern23Mode = static_cast<int>(core->GetPattern23Mode());
    if (ImGui::RadioButton("静态模式##p23", &pattern23Mode, 0)) {
        core->SetPattern23Mode(HBCore::Pattern23Mode::STATIC);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("动态模式##p23", &pattern23Mode, 1)) {
        core->SetPattern23Mode(HBCore::Pattern23Mode::DYNAMIC);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("静态模式: 直接替换12字节ID");
        ImGui::Text("动态模式: 根据偏移计算长度，可能调整包长度");
        ImGui::EndTooltip();
    }

    // 动态模式偏移（仅在动态模式下显示）
    if (core->GetPattern23Mode() == HBCore::Pattern23Mode::DYNAMIC) {
        int dynamicOffset = core->GetPattern23DynamicOffset();
        if (ImGui::InputInt("动态偏移位置", &dynamicOffset)) {
            core->SetPattern23DynamicOffset(dynamicOffset);
        }
    }

    ImGui::Spacing();

    // 替换模式选择
    ImGui::Text("数据池选择模式:");
    int replaceMode = static_cast<int>(core->GetReplaceMode());
    if (ImGui::RadioButton("随机##rm", &replaceMode, 0)) {
        core->SetReplaceMode(HBCore::ReplaceMode::RANDOM);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("顺序##rm", &replaceMode, 1)) {
        core->SetReplaceMode(HBCore::ReplaceMode::SEQUENTIAL);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("固定##rm", &replaceMode, 2)) {
        core->SetReplaceMode(HBCore::ReplaceMode::FIXED);
    }

    // 数据顺序模式（仅在顺序模式下显示）
    if (core->GetReplaceMode() == HBCore::ReplaceMode::SEQUENTIAL) {
        ImGui::Text("数据获取顺序:");
        int orderMode = static_cast<int>(core->GetDataOrderMode());
        if (ImGui::RadioButton("升序(FIFO)##om", &orderMode, 0)) {
            core->SetDataOrderMode(HBCore::DataOrderMode::Ascending);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("降序(LIFO)##om", &orderMode, 1)) {
            core->SetDataOrderMode(HBCore::DataOrderMode::Descending);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("升序(FIFO): 先进先出，优先使用最旧的数据");
            ImGui::Text("降序(LIFO): 后进先出，优先使用最新的数据");
            ImGui::EndTooltip();
        }

        // 顺序模式配置（按ID长度）
        ImGui::Spacing();
        if (ImGui::TreeNode("顺序模式配置（按ID长度）##seqConfig")) {
            static int newSeqLength = 12;
            static int newSeqStartId = 1;
            static int newSeqEndId = 1000;

            ImGui::Text("添加新配置:");
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("ID长度##newSeqLen", &newSeqLength);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("起始ID##newSeqStart", &newSeqStartId);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("结束ID##newSeqEnd", &newSeqEndId);
            ImGui::SameLine();
            if (ImGui::Button("添加##addSeqCfg")) {
                if (newSeqLength > 0 && newSeqStartId > 0 && newSeqEndId >= newSeqStartId) {
                    HBCore::SequentialConfig cfg(newSeqStartId, newSeqEndId, true);
                    core->SetSequentialConfig(newSeqLength, cfg);
                }
            }

            ImGui::Spacing();

            // 显示现有配置
            auto configs = core->GetAllSequentialConfigs();
            if (!configs.empty()) {
                if (ImGui::BeginTable("##seqConfigTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("ID长度", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("起始ID", ImGuiTableColumnFlags_WidthFixed, 70);
                    ImGui::TableSetupColumn("结束ID", ImGuiTableColumnFlags_WidthFixed, 70);
                    ImGui::TableSetupColumn("数据量", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableHeadersRow();

                    for (auto& pair : configs) {
                        int length = pair.first;
                        auto& cfg = pair.second;

                        ImGui::TableNextRow();
                        ImGui::PushID(length);

                        ImGui::TableNextColumn();
                        ImGui::Text("%d", length);

                        ImGui::TableNextColumn();
                        int startId = cfg.startId;
                        ImGui::SetNextItemWidth(60);
                        if (ImGui::InputInt("##start", &startId, 0, 0)) {
                            cfg.startId = startId;
                            core->SetSequentialConfig(length, cfg);
                        }

                        ImGui::TableNextColumn();
                        int endId = cfg.endId;
                        ImGui::SetNextItemWidth(60);
                        if (ImGui::InputInt("##end", &endId, 0, 0)) {
                            cfg.endId = endId;
                            core->SetSequentialConfig(length, cfg);
                        }

                        ImGui::TableNextColumn();
                        ImGui::Text("%d", cfg.currentCount);

                        ImGui::TableNextColumn();
                        bool enabled = cfg.enabled;
                        if (ImGui::Checkbox("##en", &enabled)) {
                            cfg.enabled = enabled;
                            core->SetSequentialConfig(length, cfg);
                        }

                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("删除")) {
                            core->RemoveSequentialConfig(length);
                        }

                        ImGui::PopID();
                    }

                    ImGui::EndTable();
                }

                if (ImGui::Button("刷新数据量##refreshSeqCounts")) {
                    core->UpdateSequentialConfigCounts();
                }
                ImGui::SameLine();
                if (ImGui::Button("清空配置##clearSeqCfg")) {
                    core->ClearSequentialConfigs();
                }
            } else {
                ImGui::TextDisabled("暂无配置，请添加");
            }

            ImGui::TreePop();
        }
    }

    ImGui::Spacing();

    // 特征开关
    ImGui::Text("特征开关:");

    bool enable62 = core->GetEnable62Pattern();
    if (ImGui::Checkbox("启用62特征替换", &enable62)) {
        core->SetEnable62Pattern(enable62);
    }

    bool enable09_62Complement = core->GetEnable09_62Complement();
    if (ImGui::Checkbox("启用09/62互补逻辑", &enable09_62Complement)) {
        core->SetEnable09_62Complement(enable09_62Complement);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("启用后，如果包中存在09特征，则跳过62特征处理");
        ImGui::EndTooltip();
    }

    bool enableCRC32 = core->GetEnableCRC32AutoCalc();
    if (ImGui::Checkbox("启用CRC32自动计算", &enableCRC32)) {
        core->SetEnableCRC32AutoCalc(enableCRC32);
    }

    ImGui::Spacing();

    // 同算法模式
    ImGui::Text("替换计数分组:");
    bool isSameAlgorithm = core->GetSameAlgorithmMode();
    if (ImGui::RadioButton("按GameID分组", isSameAlgorithm)) {
        core->SetSameAlgorithmMode(true);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("按用户名分组", !isSameAlgorithm)) {
        core->SetSameAlgorithmMode(false);
    }
}

// ==================== 23偏移规则UI ====================
static void RenderPattern23OffsetRulesUI(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("23特征偏移替换规则");
    ImGui::Separator();

    // 规则列表
    auto rules = core->GetPattern23OffsetRules();

    if (ImGui::BeginTable("Pattern23Rules", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("长度", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        for (auto& rule : rules) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool enabled = rule.enabled;
            std::string checkboxId = "##p23en" + std::to_string(rule.id);
            if (ImGui::Checkbox(checkboxId.c_str(), &enabled)) {
                rule.enabled = enabled;
                core->UpdatePattern23OffsetRule(rule.id, rule);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%d", rule.offset);

            ImGui::TableNextColumn();
            ImGui::Text("%d", rule.length);

            ImGui::TableNextColumn();
            std::string editBtnId = "编辑##p23edit" + std::to_string(rule.id);
            if (ImGui::SmallButton(editBtnId.c_str())) {
                g_heartbeatUIState.showPattern23RuleEditor = true;
                g_heartbeatUIState.isEditingPattern23Rule = true;
                g_heartbeatUIState.editingPattern23RuleId = rule.id;
                strncpy_s(g_heartbeatUIState.pattern23RuleNameBuf, rule.name.c_str(), sizeof(g_heartbeatUIState.pattern23RuleNameBuf) - 1);
                g_heartbeatUIState.pattern23RuleOffset = rule.offset;
                g_heartbeatUIState.pattern23RuleLength = rule.length;
                g_heartbeatUIState.pattern23RuleEnabled = rule.enabled;
            }
            ImGui::SameLine();
            std::string delBtnId = "删除##p23del" + std::to_string(rule.id);
            if (ImGui::SmallButton(delBtnId.c_str())) {
                core->RemovePattern23OffsetRule(rule.id);
            }
        }

        ImGui::EndTable();
    }

    if (ImGui::Button("添加23偏移规则")) {
        g_heartbeatUIState.showPattern23RuleEditor = true;
        g_heartbeatUIState.isEditingPattern23Rule = false;
        g_heartbeatUIState.editingPattern23RuleId = -1;
        memset(g_heartbeatUIState.pattern23RuleNameBuf, 0, sizeof(g_heartbeatUIState.pattern23RuleNameBuf));
        g_heartbeatUIState.pattern23RuleOffset = 0;
        g_heartbeatUIState.pattern23RuleLength = 12;
        g_heartbeatUIState.pattern23RuleEnabled = true;
    }

    // 规则编辑弹窗
    if (g_heartbeatUIState.showPattern23RuleEditor) {
        ImGui::OpenPopup("23偏移规则编辑器");
    }

    if (ImGui::BeginPopupModal("23偏移规则编辑器", &g_heartbeatUIState.showPattern23RuleEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("规则名称##p23name", g_heartbeatUIState.pattern23RuleNameBuf, sizeof(g_heartbeatUIState.pattern23RuleNameBuf));
        ImGui::InputInt("偏移位置##p23offset", &g_heartbeatUIState.pattern23RuleOffset);
        ImGui::InputInt("替换长度##p23len", &g_heartbeatUIState.pattern23RuleLength);
        ImGui::Checkbox("启用##p23enabled", &g_heartbeatUIState.pattern23RuleEnabled);

        ImGui::Spacing();

        if (ImGui::Button("确定##p23ok", ImVec2(120, 0))) {
            HBCore::Pattern23OffsetRule rule;
            rule.name = g_heartbeatUIState.pattern23RuleNameBuf;
            rule.offset = g_heartbeatUIState.pattern23RuleOffset;
            rule.length = g_heartbeatUIState.pattern23RuleLength;
            rule.enabled = g_heartbeatUIState.pattern23RuleEnabled;

            if (g_heartbeatUIState.isEditingPattern23Rule) {
                rule.id = g_heartbeatUIState.editingPattern23RuleId;
                core->UpdatePattern23OffsetRule(rule.id, rule);
            }
            else {
                core->AddPattern23OffsetRule(rule);
            }

            g_heartbeatUIState.showPattern23RuleEditor = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##p23cancel", ImVec2(120, 0))) {
            g_heartbeatUIState.showPattern23RuleEditor = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ==================== 09偏移规则UI ====================
static void RenderPattern09OffsetRulesUI(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("09特征偏移替换规则");
    ImGui::Separator();

    auto rules = core->GetPattern09OffsetRules();

    if (ImGui::BeginTable("Pattern09Rules", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("长度", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        for (auto& rule : rules) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool enabled = rule.enabled;
            std::string checkboxId = "##p09en" + std::to_string(rule.id);
            if (ImGui::Checkbox(checkboxId.c_str(), &enabled)) {
                rule.enabled = enabled;
                core->UpdatePattern09OffsetRule(rule.id, rule);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%d", rule.offset);

            ImGui::TableNextColumn();
            ImGui::Text("%d", rule.length);

            ImGui::TableNextColumn();
            std::string editBtnId = "编辑##p09edit" + std::to_string(rule.id);
            if (ImGui::SmallButton(editBtnId.c_str())) {
                g_heartbeatUIState.showPattern09RuleEditor = true;
                g_heartbeatUIState.isEditingPattern09Rule = true;
                g_heartbeatUIState.editingPattern09RuleId = rule.id;
                strncpy_s(g_heartbeatUIState.pattern09RuleNameBuf, rule.name.c_str(), sizeof(g_heartbeatUIState.pattern09RuleNameBuf) - 1);
                g_heartbeatUIState.pattern09RuleOffset = rule.offset;
                g_heartbeatUIState.pattern09RuleLength = rule.length;
                g_heartbeatUIState.pattern09RuleEnabled = rule.enabled;
            }
            ImGui::SameLine();
            std::string delBtnId = "删除##p09del" + std::to_string(rule.id);
            if (ImGui::SmallButton(delBtnId.c_str())) {
                core->RemovePattern09OffsetRule(rule.id);
            }
        }

        ImGui::EndTable();
    }

    if (ImGui::Button("添加09偏移规则")) {
        g_heartbeatUIState.showPattern09RuleEditor = true;
        g_heartbeatUIState.isEditingPattern09Rule = false;
        g_heartbeatUIState.editingPattern09RuleId = -1;
        memset(g_heartbeatUIState.pattern09RuleNameBuf, 0, sizeof(g_heartbeatUIState.pattern09RuleNameBuf));
        g_heartbeatUIState.pattern09RuleOffset = 0;
        g_heartbeatUIState.pattern09RuleLength = 12;
        g_heartbeatUIState.pattern09RuleEnabled = true;
    }

    // 规则编辑弹窗
    if (g_heartbeatUIState.showPattern09RuleEditor) {
        ImGui::OpenPopup("09偏移规则编辑器");
    }

    if (ImGui::BeginPopupModal("09偏移规则编辑器", &g_heartbeatUIState.showPattern09RuleEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("规则名称##p09name", g_heartbeatUIState.pattern09RuleNameBuf, sizeof(g_heartbeatUIState.pattern09RuleNameBuf));
        ImGui::InputInt("偏移位置##p09offset", &g_heartbeatUIState.pattern09RuleOffset);
        ImGui::InputInt("替换长度##p09len", &g_heartbeatUIState.pattern09RuleLength);
        ImGui::Checkbox("启用##p09enabled", &g_heartbeatUIState.pattern09RuleEnabled);

        ImGui::Spacing();

        if (ImGui::Button("确定##p09ok", ImVec2(120, 0))) {
            HBCore::Pattern09OffsetRule rule;
            rule.name = g_heartbeatUIState.pattern09RuleNameBuf;
            rule.offset = g_heartbeatUIState.pattern09RuleOffset;
            rule.length = g_heartbeatUIState.pattern09RuleLength;
            rule.enabled = g_heartbeatUIState.pattern09RuleEnabled;

            if (g_heartbeatUIState.isEditingPattern09Rule) {
                rule.id = g_heartbeatUIState.editingPattern09RuleId;
                core->UpdatePattern09OffsetRule(rule.id, rule);
            }
            else {
                core->AddPattern09OffsetRule(rule);
            }

            g_heartbeatUIState.showPattern09RuleEditor = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##p09cancel", ImVec2(120, 0))) {
            g_heartbeatUIState.showPattern09RuleEditor = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ==================== 62偏移规则UI ====================
static void RenderPattern62OffsetRulesUI(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("62特征偏移替换规则");
    ImGui::Separator();

    // 62特征开关
    bool enable62Pattern = core->GetEnable62Pattern();
    if (ImGui::Checkbox("启用62特征替换", &enable62Pattern)) {
        core->SetEnable62Pattern(enable62Pattern);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("启用后，将替换01 0A 00 62特征后的数据");
        ImGui::EndTooltip();
    }

    // 09/62互补开关
    bool enable09_62Complement = core->GetEnable09_62Complement();
    if (ImGui::Checkbox("启用09/62互补", &enable09_62Complement)) {
        core->SetEnable09_62Complement(enable09_62Complement);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("启用后，如果数据池中09数据为空则用62数据填充，反之亦然");
        ImGui::EndTooltip();
    }

    if (!enable62Pattern) {
        ImGui::TextDisabled("62特征替换已禁用");
        return;
    }

    ImGui::Spacing();

    // 规则列表
    auto rules = core->GetPattern62OffsetRules();

    if (ImGui::BeginTable("Pattern62OffsetRules", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("长度", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        for (auto& rule : rules) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool enabled = rule.enabled;
            if (ImGui::Checkbox(("##p62en" + std::to_string(rule.id)).c_str(), &enabled)) {
                rule.enabled = enabled;
                core->UpdatePattern62OffsetRule(rule.id, rule);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%d", rule.offset);

            ImGui::TableNextColumn();
            ImGui::Text("%d", rule.length);

            ImGui::TableNextColumn();
            if (ImGui::SmallButton(("编辑##p62edit" + std::to_string(rule.id)).c_str())) {
                g_heartbeatUIState.showPattern62RuleEditor = true;
                g_heartbeatUIState.isEditingPattern62Rule = true;
                g_heartbeatUIState.editingPattern62RuleId = rule.id;
                strncpy_s(g_heartbeatUIState.pattern62RuleNameBuf, rule.name.c_str(), sizeof(g_heartbeatUIState.pattern62RuleNameBuf) - 1);
                g_heartbeatUIState.pattern62RuleOffset = rule.offset;
                g_heartbeatUIState.pattern62RuleLength = rule.length;
                g_heartbeatUIState.pattern62RuleEnabled = rule.enabled;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(("删除##p62del" + std::to_string(rule.id)).c_str())) {
                core->RemovePattern62OffsetRule(rule.id);
            }
        }

        ImGui::EndTable();
    }

    if (ImGui::Button("添加62偏移规则")) {
        g_heartbeatUIState.showPattern62RuleEditor = true;
        g_heartbeatUIState.isEditingPattern62Rule = false;
        g_heartbeatUIState.editingPattern62RuleId = -1;
        memset(g_heartbeatUIState.pattern62RuleNameBuf, 0, sizeof(g_heartbeatUIState.pattern62RuleNameBuf));
        g_heartbeatUIState.pattern62RuleOffset = 0;
        g_heartbeatUIState.pattern62RuleLength = 12;
        g_heartbeatUIState.pattern62RuleEnabled = true;
    }

    // 规则编辑弹窗
    if (g_heartbeatUIState.showPattern62RuleEditor) {
        ImGui::OpenPopup("62偏移规则编辑器");
    }

    if (ImGui::BeginPopupModal("62偏移规则编辑器", &g_heartbeatUIState.showPattern62RuleEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("规则名称##p62name", g_heartbeatUIState.pattern62RuleNameBuf, sizeof(g_heartbeatUIState.pattern62RuleNameBuf));
        ImGui::InputInt("偏移位置##p62offset", &g_heartbeatUIState.pattern62RuleOffset);
        ImGui::InputInt("替换长度##p62len", &g_heartbeatUIState.pattern62RuleLength);
        ImGui::Checkbox("启用##p62enabled", &g_heartbeatUIState.pattern62RuleEnabled);

        ImGui::Spacing();

        if (ImGui::Button("确定##p62ok", ImVec2(120, 0))) {
            HBCore::Pattern62OffsetRule rule;
            rule.name = g_heartbeatUIState.pattern62RuleNameBuf;
            rule.offset = g_heartbeatUIState.pattern62RuleOffset;
            rule.length = g_heartbeatUIState.pattern62RuleLength;
            rule.enabled = g_heartbeatUIState.pattern62RuleEnabled;

            if (g_heartbeatUIState.isEditingPattern62Rule) {
                rule.id = g_heartbeatUIState.editingPattern62RuleId;
                core->UpdatePattern62OffsetRule(rule.id, rule);
            }
            else {
                core->AddPattern62OffsetRule(rule);
            }

            g_heartbeatUIState.showPattern62RuleEditor = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##p62cancel", ImVec2(120, 0))) {
            g_heartbeatUIState.showPattern62RuleEditor = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ==================== 白名单规则UI ====================
static void RenderWhitelistRulesUI(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("伪心跳白名单规则");
    ImGui::Separator();

    // 白名单开关
    bool enableWhitelist = core->GetEnableWhitelist();
    if (ImGui::Checkbox("启用白名单", &enableWhitelist)) {
        core->SetEnableWhitelist(enableWhitelist);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("启用后，只有匹配白名单规则的包才会被替换");
        ImGui::EndTooltip();
    }

    if (!enableWhitelist) {
        ImGui::TextDisabled("白名单已禁用");
        return;
    }

    ImGui::Spacing();

    // 规则列表
    auto rules = core->GetWhitelistRules();

    if (ImGui::BeginTable("WhitelistRules", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("模式", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("命中", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        for (auto& rule : rules) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool enabled = rule.isEnabled;
            std::string checkboxId = "##wlen" + std::to_string(rule.id);
            if (ImGui::Checkbox(checkboxId.c_str(), &enabled)) {
                rule.isEnabled = enabled;
                core->UpdateWhitelistRule(rule.id, rule);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.usePatternSearch ? "模式搜索" : "绝对位置");

            ImGui::TableNextColumn();
            ImGui::Text("%llu", rule.matchCount);

            ImGui::TableNextColumn();
            std::string editBtnId = "编辑##wledit" + std::to_string(rule.id);
            if (ImGui::SmallButton(editBtnId.c_str())) {
                g_heartbeatUIState.showWhitelistRuleEditor = true;
                g_heartbeatUIState.isEditingWhitelistRule = true;
                g_heartbeatUIState.editingWhitelistRuleId = rule.id;
                strncpy_s(g_heartbeatUIState.whitelistRuleNameBuf, rule.name.c_str(), sizeof(g_heartbeatUIState.whitelistRuleNameBuf) - 1);
                strncpy_s(g_heartbeatUIState.whitelistRulePatternBuf, rule.searchPattern.c_str(), sizeof(g_heartbeatUIState.whitelistRulePatternBuf) - 1);
                g_heartbeatUIState.whitelistRuleUsePatternSearch = rule.usePatternSearch;
                g_heartbeatUIState.whitelistRuleEnabled = rule.isEnabled;
            }
            ImGui::SameLine();
            std::string delBtnId = "删除##wldel" + std::to_string(rule.id);
            if (ImGui::SmallButton(delBtnId.c_str())) {
                core->RemoveWhitelistRule(rule.id);
            }
        }

        ImGui::EndTable();
    }

    if (ImGui::Button("添加白名单规则")) {
        g_heartbeatUIState.showWhitelistRuleEditor = true;
        g_heartbeatUIState.isEditingWhitelistRule = false;
        g_heartbeatUIState.editingWhitelistRuleId = -1;
        memset(g_heartbeatUIState.whitelistRuleNameBuf, 0, sizeof(g_heartbeatUIState.whitelistRuleNameBuf));
        memset(g_heartbeatUIState.whitelistRulePatternBuf, 0, sizeof(g_heartbeatUIState.whitelistRulePatternBuf));
        g_heartbeatUIState.whitelistRuleUsePatternSearch = true;
        g_heartbeatUIState.whitelistRuleEnabled = true;
    }

    // 规则编辑弹窗
    if (g_heartbeatUIState.showWhitelistRuleEditor) {
        ImGui::OpenPopup("白名单规则编辑器");
    }

    if (ImGui::BeginPopupModal("白名单规则编辑器", &g_heartbeatUIState.showWhitelistRuleEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("规则名称##wlname", g_heartbeatUIState.whitelistRuleNameBuf, sizeof(g_heartbeatUIState.whitelistRuleNameBuf));

        ImGui::Text("搜索模式:");
        if (ImGui::RadioButton("模式搜索##wlmode", g_heartbeatUIState.whitelistRuleUsePatternSearch)) {
            g_heartbeatUIState.whitelistRuleUsePatternSearch = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("绝对位置##wlmode", !g_heartbeatUIState.whitelistRuleUsePatternSearch)) {
            g_heartbeatUIState.whitelistRuleUsePatternSearch = false;
        }

        ImGui::InputTextMultiline("匹配模式##wlpattern", g_heartbeatUIState.whitelistRulePatternBuf,
            sizeof(g_heartbeatUIState.whitelistRulePatternBuf), ImVec2(400, 100));
        ImGui::TextDisabled("格式: pos|hex,pos|hex,... 例如: 0|01 0A 00 23,4|FF");

        ImGui::Checkbox("启用##wlenabled", &g_heartbeatUIState.whitelistRuleEnabled);

        ImGui::Spacing();

        if (ImGui::Button("确定##wlok", ImVec2(120, 0))) {
            HBCore::WhitelistRule rule;
            rule.name = g_heartbeatUIState.whitelistRuleNameBuf;
            rule.searchPattern = g_heartbeatUIState.whitelistRulePatternBuf;
            rule.usePatternSearch = g_heartbeatUIState.whitelistRuleUsePatternSearch;
            rule.isEnabled = g_heartbeatUIState.whitelistRuleEnabled;

            if (g_heartbeatUIState.isEditingWhitelistRule) {
                rule.id = g_heartbeatUIState.editingWhitelistRuleId;
                core->UpdateWhitelistRule(rule.id, rule);
            }
            else {
                core->AddWhitelistRule(rule);
            }

            g_heartbeatUIState.showWhitelistRuleEditor = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##wlcancel", ImVec2(120, 0))) {
            g_heartbeatUIState.showWhitelistRuleEditor = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ==================== 黑名单规则UI ====================
static void RenderBlacklistRulesUI(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("伪心跳黑名单规则");
    ImGui::Separator();

    // 黑名单开关
    bool enableBlacklist = core->GetEnableBlacklist();
    if (ImGui::Checkbox("启用黑名单", &enableBlacklist)) {
        core->SetEnableBlacklist(enableBlacklist);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("启用后，匹配黑名单规则的包将被跳过不替换");
        ImGui::EndTooltip();
    }

    if (!enableBlacklist) {
        ImGui::TextDisabled("黑名单已禁用");
        return;
    }

    ImGui::Spacing();

    // 规则列表
    auto rules = core->GetBlacklistRules();

    if (ImGui::BeginTable("BlacklistRules", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("模式", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("命中", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        for (auto& rule : rules) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool enabled = rule.isEnabled;
            std::string checkboxId = "##blen" + std::to_string(rule.id);
            if (ImGui::Checkbox(checkboxId.c_str(), &enabled)) {
                rule.isEnabled = enabled;
                core->UpdateBlacklistRule(rule.id, rule);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.usePatternSearch ? "模式搜索" : "绝对位置");

            ImGui::TableNextColumn();
            ImGui::Text("%llu", rule.matchCount);

            ImGui::TableNextColumn();
            std::string editBtnId = "编辑##bledit" + std::to_string(rule.id);
            if (ImGui::SmallButton(editBtnId.c_str())) {
                g_heartbeatUIState.showBlacklistRuleEditor = true;
                g_heartbeatUIState.isEditingBlacklistRule = true;
                g_heartbeatUIState.editingBlacklistRuleId = rule.id;
                strncpy_s(g_heartbeatUIState.blacklistRuleNameBuf, rule.name.c_str(), sizeof(g_heartbeatUIState.blacklistRuleNameBuf) - 1);
                strncpy_s(g_heartbeatUIState.blacklistRulePatternBuf, rule.searchPattern.c_str(), sizeof(g_heartbeatUIState.blacklistRulePatternBuf) - 1);
                g_heartbeatUIState.blacklistRuleUsePatternSearch = rule.usePatternSearch;
                g_heartbeatUIState.blacklistRuleEnabled = rule.isEnabled;
            }
            ImGui::SameLine();
            std::string delBtnId = "删除##bldel" + std::to_string(rule.id);
            if (ImGui::SmallButton(delBtnId.c_str())) {
                core->RemoveBlacklistRule(rule.id);
            }
        }

        ImGui::EndTable();
    }

    if (ImGui::Button("添加黑名单规则")) {
        g_heartbeatUIState.showBlacklistRuleEditor = true;
        g_heartbeatUIState.isEditingBlacklistRule = false;
        g_heartbeatUIState.editingBlacklistRuleId = -1;
        memset(g_heartbeatUIState.blacklistRuleNameBuf, 0, sizeof(g_heartbeatUIState.blacklistRuleNameBuf));
        memset(g_heartbeatUIState.blacklistRulePatternBuf, 0, sizeof(g_heartbeatUIState.blacklistRulePatternBuf));
        g_heartbeatUIState.blacklistRuleUsePatternSearch = true;
        g_heartbeatUIState.blacklistRuleEnabled = true;
    }

    // 规则编辑弹窗
    if (g_heartbeatUIState.showBlacklistRuleEditor) {
        ImGui::OpenPopup("黑名单规则编辑器");
    }

    if (ImGui::BeginPopupModal("黑名单规则编辑器", &g_heartbeatUIState.showBlacklistRuleEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("规则名称##blname", g_heartbeatUIState.blacklistRuleNameBuf, sizeof(g_heartbeatUIState.blacklistRuleNameBuf));

        ImGui::Text("搜索模式:");
        if (ImGui::RadioButton("模式搜索##blmode", g_heartbeatUIState.blacklistRuleUsePatternSearch)) {
            g_heartbeatUIState.blacklistRuleUsePatternSearch = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("绝对位置##blmode", !g_heartbeatUIState.blacklistRuleUsePatternSearch)) {
            g_heartbeatUIState.blacklistRuleUsePatternSearch = false;
        }

        ImGui::InputTextMultiline("匹配模式##blpattern", g_heartbeatUIState.blacklistRulePatternBuf,
            sizeof(g_heartbeatUIState.blacklistRulePatternBuf), ImVec2(400, 100));
        ImGui::TextDisabled("格式: pos|hex,pos|hex,... 例如: 0|01 0A 00 23,4|FF");

        ImGui::Checkbox("启用##blenabled", &g_heartbeatUIState.blacklistRuleEnabled);

        ImGui::Spacing();

        if (ImGui::Button("确定##blok", ImVec2(120, 0))) {
            HBCore::BlacklistRule rule;
            rule.name = g_heartbeatUIState.blacklistRuleNameBuf;
            rule.searchPattern = g_heartbeatUIState.blacklistRulePatternBuf;
            rule.usePatternSearch = g_heartbeatUIState.blacklistRuleUsePatternSearch;
            rule.isEnabled = g_heartbeatUIState.blacklistRuleEnabled;

            if (g_heartbeatUIState.isEditingBlacklistRule) {
                rule.id = g_heartbeatUIState.editingBlacklistRuleId;
                core->UpdateBlacklistRule(rule.id, rule);
            }
            else {
                core->AddBlacklistRule(rule);
            }

            g_heartbeatUIState.showBlacklistRuleEditor = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##blcancel", ImVec2(120, 0))) {
            g_heartbeatUIState.showBlacklistRuleEditor = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ==================== 替换数量规则UI ====================
static void RenderReplaceCountRulesUI(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("替换数量规则");
    ImGui::Separator();

    // 规则列表
    auto rules = core->GetReplaceCountRules();

    if (ImGui::BeginTable("ReplaceCountRules", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("阈值", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("到达后策略", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("断开重置", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        for (auto& rule : rules) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool enabled = rule.enabled;
            std::string checkboxId = "##rcen" + std::to_string(rule.id);
            if (ImGui::Checkbox(checkboxId.c_str(), &enabled)) {
                rule.enabled = enabled;
                core->UpdateReplaceCountRule(rule.id, rule);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%d", rule.replaceLimit);

            ImGui::TableNextColumn();
            const char* strategyStr = rule.postLimitStrategy == HBCore::PostLimitStrategy::SendOriginal
                ? "发原包" : "循环伪/原";
            ImGui::Text("%s", strategyStr);

            ImGui::TableNextColumn();
            ImGui::Text("%s", rule.resetCountOnDisconnect ? "是" : "否");

            ImGui::TableNextColumn();
            std::string editBtnId = "编辑##rcedit" + std::to_string(rule.id);
            if (ImGui::SmallButton(editBtnId.c_str())) {
                g_heartbeatUIState.showReplaceCountRuleEditor = true;
                g_heartbeatUIState.isEditingReplaceCountRule = true;
                g_heartbeatUIState.editingReplaceCountRuleId = rule.id;
                strncpy_s(g_heartbeatUIState.replaceCountRuleNameBuf, rule.name.c_str(), sizeof(g_heartbeatUIState.replaceCountRuleNameBuf) - 1);
                g_heartbeatUIState.replaceCountRuleLimit = rule.replaceLimit;
                g_heartbeatUIState.replaceCountRulePostStrategy = static_cast<int>(rule.postLimitStrategy);
                g_heartbeatUIState.replaceCountRuleClearPool = rule.clearPoolOnReach;
                g_heartbeatUIState.replaceCountRuleApplyWpe = rule.applyWpeOnOriginalSegment;
                g_heartbeatUIState.replaceCountRuleResetOnDisconnect = rule.resetCountOnDisconnect;
                g_heartbeatUIState.replaceCountRulePostFakeN = rule.postLimitFakeN;
                g_heartbeatUIState.replaceCountRulePostOriginalN = rule.postLimitOriginalN;
                g_heartbeatUIState.replaceCountRuleEnabled = rule.enabled;
            }
            ImGui::SameLine();
            std::string delBtnId = "删除##rcdel" + std::to_string(rule.id);
            if (ImGui::SmallButton(delBtnId.c_str())) {
                core->RemoveReplaceCountRule(rule.id);
            }
        }

        ImGui::EndTable();
    }

    if (ImGui::Button("添加替换数量规则")) {
        g_heartbeatUIState.showReplaceCountRuleEditor = true;
        g_heartbeatUIState.isEditingReplaceCountRule = false;
        g_heartbeatUIState.editingReplaceCountRuleId = -1;
        memset(g_heartbeatUIState.replaceCountRuleNameBuf, 0, sizeof(g_heartbeatUIState.replaceCountRuleNameBuf));
        g_heartbeatUIState.replaceCountRuleLimit = 1;
        g_heartbeatUIState.replaceCountRulePostStrategy = 0;
        g_heartbeatUIState.replaceCountRuleClearPool = false;
        g_heartbeatUIState.replaceCountRuleApplyWpe = true;
        g_heartbeatUIState.replaceCountRuleResetOnDisconnect = true;
        g_heartbeatUIState.replaceCountRulePostFakeN = 1;
        g_heartbeatUIState.replaceCountRulePostOriginalN = 1;
        g_heartbeatUIState.replaceCountRuleEnabled = true;
    }

    // 规则编辑弹窗
    if (g_heartbeatUIState.showReplaceCountRuleEditor) {
        ImGui::OpenPopup("替换数量规则编辑器");
    }

    if (ImGui::BeginPopupModal("替换数量规则编辑器", &g_heartbeatUIState.showReplaceCountRuleEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("规则名称##rcname", g_heartbeatUIState.replaceCountRuleNameBuf, sizeof(g_heartbeatUIState.replaceCountRuleNameBuf));
        ImGui::InputInt("替换阈值##rclimit", &g_heartbeatUIState.replaceCountRuleLimit);
        if (g_heartbeatUIState.replaceCountRuleLimit < 1) g_heartbeatUIState.replaceCountRuleLimit = 1;

        ImGui::Spacing();
        ImGui::Text("到达阈值后策略:");
        if (ImGui::RadioButton("只发原包##rcstrategy", &g_heartbeatUIState.replaceCountRulePostStrategy, 0)) {}
        ImGui::SameLine();
        if (ImGui::RadioButton("循环伪包/原包##rcstrategy", &g_heartbeatUIState.replaceCountRulePostStrategy, 1)) {}

        // 循环策略参数
        if (g_heartbeatUIState.replaceCountRulePostStrategy == 1) {
            ImGui::Indent();
            ImGui::InputInt("伪包数量##rcfaken", &g_heartbeatUIState.replaceCountRulePostFakeN);
            ImGui::InputInt("原包数量##rcoriginaln", &g_heartbeatUIState.replaceCountRulePostOriginalN);
            if (g_heartbeatUIState.replaceCountRulePostFakeN < 1) g_heartbeatUIState.replaceCountRulePostFakeN = 1;
            if (g_heartbeatUIState.replaceCountRulePostOriginalN < 1) g_heartbeatUIState.replaceCountRulePostOriginalN = 1;
            ImGui::Unindent();
        }

        ImGui::Spacing();
        ImGui::Checkbox("到达时清理数据池##rcclear", &g_heartbeatUIState.replaceCountRuleClearPool);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("到达阈值时清理数据池中该账号/ID的数据（只执行一次）");
            ImGui::EndTooltip();
        }

        ImGui::Checkbox("原包段执行WPE滤镜##rcwpe", &g_heartbeatUIState.replaceCountRuleApplyWpe);
        ImGui::Checkbox("断开连接后重置计数##rcreset", &g_heartbeatUIState.replaceCountRuleResetOnDisconnect);
        ImGui::Checkbox("启用##rcenabled", &g_heartbeatUIState.replaceCountRuleEnabled);

        ImGui::Spacing();

        if (ImGui::Button("确定##rcok", ImVec2(120, 0))) {
            HBCore::ReplaceCountRule rule;
            rule.name = g_heartbeatUIState.replaceCountRuleNameBuf;
            rule.replaceLimit = g_heartbeatUIState.replaceCountRuleLimit;
            rule.postLimitStrategy = static_cast<HBCore::PostLimitStrategy>(g_heartbeatUIState.replaceCountRulePostStrategy);
            rule.clearPoolOnReach = g_heartbeatUIState.replaceCountRuleClearPool;
            rule.applyWpeOnOriginalSegment = g_heartbeatUIState.replaceCountRuleApplyWpe;
            rule.resetCountOnDisconnect = g_heartbeatUIState.replaceCountRuleResetOnDisconnect;
            rule.postLimitFakeN = g_heartbeatUIState.replaceCountRulePostFakeN;
            rule.postLimitOriginalN = g_heartbeatUIState.replaceCountRulePostOriginalN;
            rule.enabled = g_heartbeatUIState.replaceCountRuleEnabled;

            if (g_heartbeatUIState.isEditingReplaceCountRule) {
                rule.id = g_heartbeatUIState.editingReplaceCountRuleId;
                core->UpdateReplaceCountRule(rule.id, rule);
            }
            else {
                core->AddReplaceCountRule(rule);
            }

            g_heartbeatUIState.showReplaceCountRuleEditor = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##rccancel", ImVec2(120, 0))) {
            g_heartbeatUIState.showReplaceCountRuleEditor = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ==================== 固定包选择UI ====================
static void RenderFixedPacketSelectorUI(HeartbeatCore* core) {
    if (!core) return;

    // 只在固定模式下显示
    if (core->GetReplaceMode() != HBCore::ReplaceMode::FIXED) {
        return;
    }

    ImGui::Text("固定包选择");
    ImGui::Separator();

    // 当前选择列表
    auto selections = core->GetAllFixedPacketSelections();

    if (selections.empty()) {
        ImGui::TextDisabled("尚未选择任何固定包");
    }
    else {
        if (ImGui::BeginTable("FixedPackets", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("ID长度", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("包ID", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("GameID", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (auto& [length, selection] : selections) {
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::Text("%d", length);

                ImGui::TableNextColumn();
                ImGui::Text("%d", selection.packetId);

                ImGui::TableNextColumn();
                ImGui::Text("%s", selection.gameID.c_str());

                ImGui::TableNextColumn();
                std::string clearBtnId = "清除##fpclear" + std::to_string(length);
                if (ImGui::SmallButton(clearBtnId.c_str())) {
                    core->SetFixedPacketByLength(length, -1, "");
                }
            }

            ImGui::EndTable();
        }
    }

    ImGui::Spacing();

    // 选择新的固定包
    if (ImGui::Button("选择固定包...")) {
        g_heartbeatUIState.showFixedPacketSelector = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("清除所有固定包")) {
        core->ClearFixedPacketSelections();
    }

    // 固定包选择弹窗
    if (g_heartbeatUIState.showFixedPacketSelector) {
        ImGui::OpenPopup("选择固定包");
    }

    if (ImGui::BeginPopupModal("选择固定包", &g_heartbeatUIState.showFixedPacketSelector, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputInt("ID长度", &g_heartbeatUIState.fixedPacketSelectLength);
        if (g_heartbeatUIState.fixedPacketSelectLength < 1) g_heartbeatUIState.fixedPacketSelectLength = 1;

        ImGui::Spacing();
        ImGui::Text("从数据池中选择该长度的包:");

        // 获取数据库中该长度的包列表
        DatabaseManager* db = core->GetDatabase();
        if (db) {
            // 这里简化处理，实际应该从数据库查询
            ImGui::TextDisabled("(数据库查询功能需要在实际使用时实现)");
        }

        ImGui::Spacing();

        if (ImGui::Button("关闭##fpclose", ImVec2(120, 0))) {
            g_heartbeatUIState.showFixedPacketSelector = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ==================== 统计信息UI ====================
static void RenderHeartbeatStatisticsUI(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("统计信息");
    ImGui::Separator();

    ImGui::Text("总处理包数: %llu", core->GetTotalProcessed());
    ImGui::Text("成功替换数: %llu", core->GetTotalReplaced());
    ImGui::Text("跳过包数: %llu", core->GetTotalSkipped());

    ImGui::Spacing();

    if (ImGui::Button("重置统计")) {
        core->ResetStatistics();
    }
}

// ==================== 配置管理UI ====================
static void RenderHeartbeatConfigUI(HeartbeatCore* core) {
    if (!core) return;

    ImGui::Text("配置管理");
    ImGui::Separator();

    // 配置文件路径
    ImGui::InputText("配置文件路径", g_heartbeatUIState.configFilePath, sizeof(g_heartbeatUIState.configFilePath));

    ImGui::Spacing();

    if (ImGui::Button("保存配置")) {
        if (strlen(g_heartbeatUIState.configFilePath) > 0) {
            if (core->SaveConfig(g_heartbeatUIState.configFilePath)) {
                // 保存成功
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("加载配置")) {
        if (strlen(g_heartbeatUIState.configFilePath) > 0) {
            if (core->LoadConfig(g_heartbeatUIState.configFilePath)) {
                // 加载成功
            }
        }
    }
}

// ==================== 主渲染函数 ====================
static void RenderHeartbeatInstanceUI(HeartbeatCore* core, const std::string& instanceId) {
    if (!core) {
        ImGui::TextDisabled("HeartbeatCore未初始化");
        return;
    }

    // 更新当前选中的实例ID
    g_heartbeatUIState.selectedInstanceId = instanceId;

    // 使用TabBar组织各个配置区域
    if (ImGui::BeginTabBar("HeartbeatTabs", ImGuiTabBarFlags_None)) {
        // 基础配置标签页
        if (ImGui::BeginTabItem("基础配置")) {
            RenderHeartbeatBasicConfig(core);
            ImGui::EndTabItem();
        }

        // 23偏移规则标签页
        if (ImGui::BeginTabItem("23偏移规则")) {
            RenderPattern23OffsetRulesUI(core);
            ImGui::EndTabItem();
        }

        // 09偏移规则标签页
        if (ImGui::BeginTabItem("09偏移规则")) {
            RenderPattern09OffsetRulesUI(core);
            ImGui::EndTabItem();
        }

        // 62偏移规则标签页
        if (ImGui::BeginTabItem("62偏移规则")) {
            RenderPattern62OffsetRulesUI(core);
            ImGui::EndTabItem();
        }

        // 白名单标签页
        if (ImGui::BeginTabItem("白名单")) {
            RenderWhitelistRulesUI(core);
            ImGui::EndTabItem();
        }

        // 黑名单标签页
        if (ImGui::BeginTabItem("黑名单")) {
            RenderBlacklistRulesUI(core);
            ImGui::EndTabItem();
        }

        // 替换数量规则标签页
        if (ImGui::BeginTabItem("替换数量")) {
            RenderReplaceCountRulesUI(core);
            ImGui::EndTabItem();
        }

        // 固定包选择标签页（仅在固定模式下显示）
        if (core->GetReplaceMode() == HBCore::ReplaceMode::FIXED) {
            if (ImGui::BeginTabItem("固定包")) {
                RenderFixedPacketSelectorUI(core);
                ImGui::EndTabItem();
            }
        }

        // 统计信息标签页
        if (ImGui::BeginTabItem("统计")) {
            RenderHeartbeatStatisticsUI(core);
            ImGui::EndTabItem();
        }

        // 配置管理标签页
        if (ImGui::BeginTabItem("配置")) {
            RenderHeartbeatConfigUI(core);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// ==================== 外部接口函数 ====================
// 供InstanceManagerUI.inl调用的包装函数
void RenderHeartbeatInstanceConfig(HeartbeatInstance* instance, bool& needsRedraw) {
    // 单伪实例配置页：要求与“单伪原生项目”UI一致（左右子菜单/悬浮窗口/细项一致）
    SingleInstanceInterop::RenderHeartbeatInstanceUi(instance, needsRedraw);
}

// 供 SingleInstanceInterop 的“配置”子页复用：渲染 HeartbeatCore 的完整配置UI（每实例独立UI状态）
void RenderHeartbeatCoreUiForInstance(HeartbeatCore* core, const std::string& instanceId) {
    if (!core) return;
    struct UiStateScope {
        UiStateScope(const std::string& id) { g_heartbeatUIStatePtr = &HbInstUI_GetState(id); }
        ~UiStateScope() { g_heartbeatUIStatePtr = nullptr; }
    } scope(instanceId);

    RenderHeartbeatInstanceUI(core, instanceId);
}

#undef g_heartbeatUIState

#endif

