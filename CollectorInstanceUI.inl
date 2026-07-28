// ==================== 采集实例配置界面 ====================
// 此文件包含采集实例的详细配置UI

#include "InstanceManager.h"
#include "SingleInstanceInterop.h"

#if 1
static void RenderCollectorInstanceConfig_Full(CollectorInstance* instance, bool& needsRedraw);

void RenderCollectorInstanceConfig(CollectorInstance* instance, bool& needsRedraw) {
    // 单伪实例配置页：要求与“单伪原生项目”UI一致（左右子菜单/悬浮窗口/细项一致）
    SingleInstanceInterop::RenderCollectorInstanceUi(instance, needsRedraw);
    return;
#if 0
    if (!instance) {
        ImGuiGBK::Text("实例不存在");
        return;
    }

    auto info = instance->GetInfo();
    auto* collector = instance->GetCollector();

    if (!collector) {
        ImGuiGBK::Text("采集器未初始化");
        return;
    }

    // ========== 实例信息 ==========
    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "实例信息");
    ImGui::Separator();

    ImGuiGBK::Text("实例ID: %s", info.id.c_str());
    ImGuiGBK::Text("实例名称: %s", info.name.c_str());
    ImGuiGBK::Text("监听端口: %d", info.port);
    ImGuiGBK::Text("创建时间: %s", info.createTime.c_str());

    // 状态显示
    ImGuiGBK::Text("运行状态: ");
    ImGui::SameLine();
    if (info.state == InstanceState::Running) {
        ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "● 运行中");
    } else {
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "● 已停止");
    }

    ImGui::Separator();

    // ========== 统计信息 ==========
    ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "统计信息");
    ImGui::Separator();

    ImGuiGBK::Text("当前连接数: %d", info.currentConnections);
    ImGuiGBK::Text("总数据包数: %llu", info.totalPackets);
    ImGuiGBK::Text("总流量: %.2f KB", info.totalBytes / 1024.0);

    if (collector) {
        ImGuiGBK::Text("分包数: %d", collector->GetFragmentedPackets());
        ImGuiGBK::Text("粘包数: %d", collector->GetMultiPackets());
    }

    ImGui::Separator();

    // ========== WPE滤镜配置 ==========
    if (ImGui::CollapsingHeader(ImGuiText::U("WPE滤镜配置##wpeConfig"))) {
        ImGui::Indent();

        ImGuiGBK::Text("WPE滤镜功能");
        ImGui::SameLine();
        if (ImGuiGBK::Button("打开WPE滤镜页面##openWPE", ImVec2(150, 0))) {
            // TODO: 切换到WPE滤镜页面
            AB_LOG_INFO("[实例配置] 请在WPE滤镜页面配置滤镜");
        }

        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "提示: WPE滤镜是全局配置，影响所有采集实例");

        ImGui::Unindent();
    }

    ImGui::Separator();

    // ========== SOCKS5认证配置 ==========
    if (ImGui::CollapsingHeader(ImGuiText::U("SOCKS5认证配置##socksConfig"))) {
        ImGui::Indent();

        static bool enableSocksAuth = false;
        if (ImGuiGBK::Checkbox("启用SOCKS5认证##enableSocks", &enableSocksAuth)) {
            if (collector) {
                collector->SetSocks5Auth(enableSocksAuth);
                AB_LOG_INFO("[实例配置] SOCKS5认证已" + std::string(enableSocksAuth ? "启用" : "关闭"));
            }
            needsRedraw = true;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("启用后，客户端需要提供账号密码才能连接");
            ImGui::EndTooltip();
        }

        ImGui::Unindent();
    }

    ImGui::Separator();

    // ========== 二级代理配置 ==========
    if (ImGui::CollapsingHeader(ImGuiText::U("二级代理配置##proxyConfig"))) {
        ImGui::Indent();

        static bool enableSecondaryProxy = false;
        static char proxyHost[256] = "127.0.0.1";
        static char proxyPort[16] = "1080";
        static char proxyUsername[128] = "";
        static char proxyPassword[128] = "";

        if (ImGuiGBK::Checkbox("启用二级代理##enableProxy", &enableSecondaryProxy)) {
            needsRedraw = true;
        }

        if (enableSecondaryProxy) {
            ImGui::Spacing();

            ImGuiGBK::Text("代理地址:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("##proxyHost", proxyHost, sizeof(proxyHost));

            ImGuiGBK::Text("代理端口:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::InputText("##proxyPort", proxyPort, sizeof(proxyPort));

            ImGuiGBK::Text("用户名:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("##proxyUser", proxyUsername, sizeof(proxyUsername));

            ImGuiGBK::Text("密码:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("##proxyPass", proxyPassword, sizeof(proxyPassword), ImGuiInputTextFlags_Password);

            ImGui::Spacing();

            if (ImGuiGBK::Button("应用二级代理配置##applyProxy", ImVec2(150, 0))) {
                if (collector) {
                    int port = atoi(proxyPort);
                    if (port > 0 && port <= 65535) {
                        collector->SetSecondaryProxy(enableSecondaryProxy, proxyHost, port,
                            proxyUsername, proxyPassword);
                        AB_LOG_INFO("[实例配置] 二级代理配置已应用");
                        needsRedraw = true;
                    } else {
                        AB_LOG_ERROR("[实例配置] 无效的端口号");
                    }
                }
            }
        }

        ImGui::Unindent();
    }

    ImGui::Separator();

    // ========== 性能优化 ==========
    if (ImGui::CollapsingHeader(ImGuiText::U("性能优化##perfConfig"))) {
        ImGui::Indent();

        static bool enableDisconnectAutoClear = false;
        if (ImGuiGBK::Checkbox("断开自动清理内存池##autoClear", &enableDisconnectAutoClear)) {
            if (collector) {
                collector->SetDisconnectClearEnabled(enableDisconnectAutoClear);
                AB_LOG_INFO("[实例配置] 断开自动清理已" + std::string(enableDisconnectAutoClear ? "启用" : "关闭"));
            }
            needsRedraw = true;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("勾选后，用户断开连接时自动清理该用户的数据包");
            ImGui::EndTooltip();
        }

        ImGui::Unindent();
    }

    ImGui::Separator();

    // ========== 操作按钮 ==========
    if (info.state == InstanceState::Running) {
        if (ImGuiGBK::Button("停止实例##stopInst", ImVec2(120, 0))) {
            instance->Stop();
            AB_LOG_INFO("[实例配置] 实例已停止: " + info.id);
            needsRedraw = true;
        }
    } else {
        if (ImGuiGBK::Button("启动实例##startInst", ImVec2(120, 0))) {
            instance->Start();
            AB_LOG_INFO("[实例配置] 实例已启动: " + info.id);
            needsRedraw = true;
        }
    }

    ImGui::SameLine();
    if (ImGuiGBK::Button("刷新数据##refreshData", ImVec2(120, 0))) {
        needsRedraw = true;
    }
#endif
}

// ==================== 采集实例配置界面（完整面板）====================
// 说明：
// - 本文件用于 InstanceManager 的“实例配置”模式
// - 以 PacketCollector 的实例配置为中心，尽量对齐新伪心跳的采集端核心配置项

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

class DatabaseManager;
extern DatabaseManager* g_database;

namespace {
struct CollectorInstanceUIState {
    // 存储模式和特殊ID过滤
    bool storageModeLoaded = false;
    int storageMode = 0;  // 0=Database, 1=Memory
    bool allowCollect00ID = false;
    bool allowCollectOBID = false;

    // 过滤
    bool filterLoaded = false;
    int filterType = 0;  // FilterType: 0=none,1=ip,2=domain,3=port
    char filterValue[256] = "";

    // 二级代理
    bool proxyLoaded = false;
    bool enableSecondaryProxy = false;
    char proxyHost[256] = "127.0.0.1";
    int proxyPort = 1080;
    char proxyUsername[128] = "";
    char proxyPassword[128] = "";

    // 账号管理
    char accountUsername[64] = "";
    char accountPassword[64] = "";
    char accountExpireTime[32] = ""; // YYYY-MM-DD HH:MM:SS
    int accountMaxConnections = 1;
    bool accountEnabled = true;

    // 性能/线程池
    bool perfLoaded = false;
    int threadPoolMode = 0; // 0=Traditional,1=Blocking,2=IOCP
    int whitelistPoolSize = 10;
    int normalPoolSize = 50;
    int iocpMaxWhitelist = 2000;
    int iocpMaxNormal = 500;
    bool disconnectAutoClear = true;

    // 包头类型/62特征采集
    bool headerLoaded = false;
    bool disablePacketHeaderFilter = false;
    bool enableCollect62Pattern = false;
    char collectorPacketTypesBuf[256] = "1,2,3,4";

    // AntiCC
    bool antiCcLoaded = false;
    AntiCCConfig antiCcCfg{};
    char antiCcBlacklistInput[64] = "";
    char antiCcWhitelistInput[64] = "";

    // 采集白名单
    bool collectorWhitelistLoaded = false;
    bool enableCollectorWhitelist = false;
    int editingWhitelistRuleId = -1;
    char whitelistRuleName[64] = "";
    char whitelistRulePattern[256] = "";
    bool whitelistRuleUsePatternSearch = true;
    bool whitelistRuleEnabled = true;
    bool showWhitelistEditor = false;

    // 采集黑名单
    bool collectorBlacklistLoaded = false;
    bool enableCollectorBlacklist = false;
    int editingBlacklistRuleId = -1;
    char blacklistRuleName[64] = "";
    char blacklistRulePattern[256] = "";
    bool blacklistRuleUsePatternSearch = true;
    bool blacklistRuleEnabled = true;
    bool showBlacklistEditor = false;
};

static std::map<std::string, CollectorInstanceUIState> g_collectorInstUiStates;

static CollectorInstanceUIState& CollectorInstUI_GetState(const std::string& instanceId) {
    return g_collectorInstUiStates[instanceId];
}

static int CollectorInstUI_ModeToInt(PacketCollector::ThreadPoolMode mode) {
    switch (mode) {
    case PacketCollector::ThreadPoolMode::TRADITIONAL: return 0;
    case PacketCollector::ThreadPoolMode::BLOCKING: return 1;
    case PacketCollector::ThreadPoolMode::IOCP: return 2;
    default: return 0;
    }
}

static PacketCollector::ThreadPoolMode CollectorInstUI_IntToMode(int v) {
    switch (v) {
    case 1: return PacketCollector::ThreadPoolMode::BLOCKING;
    case 2: return PacketCollector::ThreadPoolMode::IOCP;
    default: return PacketCollector::ThreadPoolMode::TRADITIONAL;
    }
}

static void CollectorInstUI_LoadOnce(CollectorInstanceUIState& ui, PacketCollector* collector) {
    if (!collector) return;

    if (!ui.filterLoaded) {
        FilterConfig cfg = collector->GetFilter();
        ui.filterType = static_cast<int>(cfg.type);
        std::snprintf(ui.filterValue, sizeof(ui.filterValue), "%s", cfg.value.c_str());
        ui.filterLoaded = true;
    }

    if (!ui.proxyLoaded) {
        ui.enableSecondaryProxy = collector->IsSecondaryProxyEnabled();
        std::snprintf(ui.proxyHost, sizeof(ui.proxyHost), "%s", collector->GetSecondaryProxyHost().c_str());
        ui.proxyPort = collector->GetSecondaryProxyPort();
        std::snprintf(ui.proxyUsername, sizeof(ui.proxyUsername), "%s", collector->GetSecondaryProxyUsername().c_str());
        std::snprintf(ui.proxyPassword, sizeof(ui.proxyPassword), "%s", collector->GetSecondaryProxyPassword().c_str());
        ui.proxyLoaded = true;
    }

    if (!ui.perfLoaded) {
        ui.threadPoolMode = CollectorInstUI_ModeToInt(collector->GetThreadPoolMode());
        ui.whitelistPoolSize = collector->GetWhitelistPoolSize();
        ui.normalPoolSize = collector->GetNormalPoolSize();
        ui.iocpMaxWhitelist = collector->GetIOCPMaxWhitelistConnections();
        ui.iocpMaxNormal = collector->GetIOCPMaxNormalConnections();
        ui.disconnectAutoClear = collector->IsDisconnectClearEnabled();
        ui.perfLoaded = true;
    }

    if (!ui.antiCcLoaded) {
        ui.antiCcCfg = collector->GetAntiCCConfig();
        ui.antiCcLoaded = true;
    }
}

static void CollectorUI_RenderInfoStats(const InstanceInfo& info, PacketCollector* collector);
static void CollectorUI_RenderStorageMode(CollectorInstanceUIState& ui, CollectorInstance* instance, bool& needsRedraw);
static void CollectorUI_RenderCollectorWhitelist(CollectorInstanceUIState& ui, CollectorInstance* instance, bool& needsRedraw);
static void CollectorUI_RenderCollectorBlacklist(CollectorInstanceUIState& ui, CollectorInstance* instance, bool& needsRedraw);
static void CollectorUI_RenderFilter(CollectorInstanceUIState& ui, PacketCollector* collector, bool& needsRedraw);
static void CollectorUI_RenderSocksAuth(CollectorInstanceUIState& ui, PacketCollector* collector, bool& needsRedraw);
static void CollectorUI_RenderSecondaryProxy(CollectorInstanceUIState& ui, PacketCollector* collector, bool& needsRedraw);
static void CollectorUI_RenderPerformance(CollectorInstanceUIState& ui, CollectorInstance* instance, PacketCollector* collector, const InstanceInfo& info, bool& needsRedraw);
static void CollectorUI_RenderAntiCC(CollectorInstanceUIState& ui, PacketCollector* collector, const InstanceInfo& info, bool& needsRedraw);
static void CollectorUI_RenderGameIdBindings(PacketCollector* collector);
static void CollectorUI_RenderControls(CollectorInstance* instance, const InstanceInfo& info, bool& needsRedraw);
static void CollectorUI_RenderPacketHeaderConfig(CollectorInstanceUIState& ui, CollectorInstance* instance, bool& needsRedraw);
} // namespace

namespace {
static void CollectorUI_RenderAntiCC(CollectorInstanceUIState& ui, PacketCollector* collector, const InstanceInfo& info, bool& needsRedraw) {
    if (!collector) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("AntiCC（防CC）##antiCc"))) {
        return;
    }

    ImGui::Indent();

    bool antiEnabled = collector->IsAntiCCEnabled();
    if (ImGuiGBK::Checkbox("启用AntiCC##antiEnable", &antiEnabled)) {
        collector->SetAntiCCEnabled(antiEnabled);
        AB_LOG_INFO("[采集实例] AntiCC已" + std::string(antiEnabled ? "启用" : "关闭"));
        needsRedraw = true;
    }

    ImGuiGBK::Text("拦截次数: %d", collector->GetAntiCCBlockedCount());
    ImGuiGBK::Text("总连接数: %d", collector->GetAntiCCTotalConnections());
    ImGuiGBK::Text("攻击状态: %s", collector->IsUnderAttack() ? "疑似攻击" : "正常");

    ImGui::Separator();

    if (ImGuiGBK::Button("刷新当前配置##antiReload", ImVec2(160, 0))) {
        ui.antiCcCfg = collector->GetAntiCCConfig();
        needsRedraw = true;
    }

    ImGui::Separator();

    ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "配置");

    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("时间窗口(秒)##antiWindow", &ui.antiCcCfg.timeWindowSeconds);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("窗口最大请求##antiMaxReq", &ui.antiCcCfg.maxRequestsInWindow);

    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("封禁时长(秒)##antiBan", &ui.antiCcCfg.banTimeSeconds);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("最大连接数##antiMaxConn", &ui.antiCcCfg.maxConnections);

    ImGui::Checkbox("使用黑名单##antiUseBL", &ui.antiCcCfg.useBlacklist);
    ImGui::SameLine();
    ImGui::Checkbox("使用白名单##antiUseWL", &ui.antiCcCfg.useWhitelist);
    ImGui::SameLine();
    ImGui::Checkbox("拦截非SOCKS连接##antiBlockNonSocks", &ui.antiCcCfg.blockNonSocks);

    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("白名单持续(秒)##antiWlDur", &ui.antiCcCfg.whitelistDuration);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("认证失败封禁(秒)##antiAuthFail", &ui.antiCcCfg.authFailBanTime);

    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("无认证封禁(秒)##antiNoAuth", &ui.antiCcCfg.noAuthBanTime);
    ImGui::SameLine();
    ImGui::Checkbox("使用防火墙(需管理员)##antiFirewall", &ui.antiCcCfg.useFirewall);

    ImGui::Checkbox("速率限制##antiRate", &ui.antiCcCfg.rateLimitEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("限制(次/秒)##antiRateVal", &ui.antiCcCfg.rateLimit);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("窗口(秒)##antiRateWin", &ui.antiCcCfg.rateTimeWindow);

    if (ImGuiGBK::Button("应用AntiCC配置##applyAntiCfg", ImVec2(160, 0))) {
        ui.antiCcCfg.listenPort = info.port;
        collector->SetAntiCCConfig(ui.antiCcCfg);
        collector->SetAntiCCEnabled(antiEnabled);
        AB_LOG_INFO("[采集实例] 已应用AntiCC配置");
        needsRedraw = true;
    }

    ImGui::Separator();

    // 白名单/黑名单管理
    ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "黑白名单");

    ImGuiGBK::Text("黑名单IP:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##antiBlInput", ui.antiCcBlacklistInput, sizeof(ui.antiCcBlacklistInput));
    ImGui::SameLine();
    if (ImGuiGBK::Button("加入黑名单##antiAddBL", ImVec2(120, 0))) {
        if (std::strlen(ui.antiCcBlacklistInput) > 0) {
            collector->AddToBlacklist(ui.antiCcBlacklistInput);
            ui.antiCcBlacklistInput[0] = '\0';
            AB_LOG_INFO("[采集实例] 已加入黑名单");
            needsRedraw = true;
        }
    }

    ImGuiGBK::Text("白名单IP:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##antiWlInput", ui.antiCcWhitelistInput, sizeof(ui.antiCcWhitelistInput));
    ImGui::SameLine();
    if (ImGuiGBK::Button("加入白名单##antiAddWL", ImVec2(120, 0))) {
        if (std::strlen(ui.antiCcWhitelistInput) > 0) {
            collector->AddToWhitelist(ui.antiCcWhitelistInput);
            ui.antiCcWhitelistInput[0] = '\0';
            AB_LOG_INFO("[采集实例] 已加入白名单");
            needsRedraw = true;
        }
    }

    if (g_database) {
        if (ImGuiGBK::Button("从数据库加载白名单##antiLoadDb", ImVec2(180, 0))) {
            collector->SetAntiCCDatabaseManager(g_database);
            collector->LoadAntiCCWhitelistFromDB();
            AB_LOG_INFO("[采集实例] 已从数据库加载白名单");
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGuiGBK::Button("清理非活跃白名单##antiCleanup", ImVec2(180, 0))) {
            collector->CleanupAntiCCInactiveWhitelist();
            AB_LOG_INFO("[采集实例] 已触发白名单清理");
            needsRedraw = true;
        }
    } else {
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "数据库未初始化：白名单持久化按钮不可用");
    }

    auto bl = collector->GetBlacklist();
    auto wl = collector->GetWhitelist();

    if (ImGui::BeginTable("AntiLists", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
        ImGui::TableSetupColumn("黑名单", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("白名单", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const size_t maxRows = std::max(bl.size(), wl.size());
        for (size_t i = 0; i < maxRows; i++) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (i < bl.size()) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("%s", bl[i].c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("移除##rmBL")) {
                    collector->RemoveFromBlacklist(bl[i]);
                    needsRedraw = true;
                }
                ImGui::PopID();
            }

            ImGui::TableNextColumn();
            if (i < wl.size()) {
                ImGui::PushID(static_cast<int>(i + 10000));
                ImGui::Text("%s", wl[i].c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("移除##rmWL")) {
                    collector->RemoveFromWhitelist(wl[i]);
                    needsRedraw = true;
                }
                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }

    ImGui::Unindent();
}
} // namespace

namespace {
static void CollectorUI_RenderPerformance(CollectorInstanceUIState& ui, CollectorInstance* instance, PacketCollector* collector, const InstanceInfo& info, bool& needsRedraw) {
    if (!collector) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("性能与线程池##perfConfig"))) {
        return;
    }

    ImGui::Indent();

    if (info.state == InstanceState::Running) {
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "提示：线程模型/线程池/IOCP参数通常需要停止实例后再生效。");
    }

    const char* modes[] = { "传统模式（每连接线程）", "阻塞式线程池", "IOCP高性能" };
    ImGuiGBK::Text("线程模型:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("##threadMode", &ui.threadPoolMode, modes, 3);

    if (ImGuiGBK::Button("应用线程模型##applyThreadMode", ImVec2(160, 0))) {
        collector->SetThreadPoolMode(CollectorInstUI_IntToMode(ui.threadPoolMode));
        AB_LOG_INFO("[采集实例] 已应用线程模型");
        needsRedraw = true;
    }

    ImGui::Spacing();

    if (ui.threadPoolMode == 1) {
        ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "阻塞式线程池参数");
        ImGuiGBK::Text("白名单线程数:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##wlPool", &ui.whitelistPoolSize);

        ImGuiGBK::Text("普通线程数:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##nlPool", &ui.normalPoolSize);

        if (ImGuiGBK::Button("应用线程池大小##applyPoolSize", ImVec2(160, 0))) {
            collector->SetThreadPoolSizes(std::max(1, ui.whitelistPoolSize), std::max(1, ui.normalPoolSize));
            AB_LOG_INFO("[采集实例] 已应用线程池大小");
            needsRedraw = true;
        }

        ImGuiGBK::Text("白名单处理: %d, 队列: %d", collector->GetWhitelistPoolProcessed(), collector->GetWhitelistQueueSize());
        ImGuiGBK::Text("普通处理: %d, 队列: %d", collector->GetNormalPoolProcessed(), collector->GetNormalQueueSize());
    } else if (ui.threadPoolMode == 2) {
        ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "IOCP参数");
        ImGuiGBK::Text("白名单最大连接:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##iocpWlMax", &ui.iocpMaxWhitelist);

        ImGuiGBK::Text("普通最大连接:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##iocpNlMax", &ui.iocpMaxNormal);

        if (ImGuiGBK::Button("应用IOCP连接上限##applyIocpMax", ImVec2(180, 0))) {
            collector->SetIOCPMaxConnections(std::max(1, ui.iocpMaxWhitelist), std::max(1, ui.iocpMaxNormal));
            AB_LOG_INFO("[采集实例] 已应用IOCP连接上限");
            needsRedraw = true;
        }

        ImGuiGBK::Text("IOCP连接数: 白名单 %d / 普通 %d / 总 %d",
            collector->GetIOCPWhitelistConnCount(), collector->GetIOCPNormalConnCount(), collector->GetIOCPTotalConnCount());
    }

    bool disconnectClear = collector->IsDisconnectClearEnabled();
    if (ImGuiGBK::Checkbox("断开自动清理GameID绑定##autoClear", &disconnectClear)) {
        if (instance) {
            instance->SetDisconnectAutoClear(disconnectClear);
        }
        else {
            collector->SetDisconnectClearEnabled(disconnectClear);
        }
        ui.disconnectAutoClear = disconnectClear;
        AB_LOG_INFO("[采集实例] 断开自动清理已" + std::string(disconnectClear ? "启用" : "关闭"));
        needsRedraw = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGuiGBK::Text("启用后，用户断开连接时：");
        ImGuiGBK::Text("1. 自动解绑GameID与会话的关联");
        ImGuiGBK::Text("2. 自动清理该GameID在内存池中的数据");
        ImGui::EndTooltip();
    }

    ImGui::Unindent();
}

static void CollectorUI_RenderGameIdBindings(PacketCollector* collector) {
    if (!collector) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("GameID绑定（账号->会话）##gameidBinding"))) {
        return;
    }

    ImGui::Indent();

    auto bindings = collector->GetAllGameIDBindings();
    ImGuiGBK::Text("绑定数量: %d", static_cast<int>(bindings.size()));

    if (ImGui::BeginTable("GameIdBindingTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
        ImGui::TableSetupColumn("用户名", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("GameID", ImGuiTableColumnFlags_WidthFixed, 240.0f);
        ImGui::TableHeadersRow();

        for (const auto& kv : bindings) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", kv.first.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s", kv.second.c_str());
        }

        ImGui::EndTable();
    }

    ImGui::Unindent();
}
} // namespace

namespace {
static void CollectorUI_RenderSocksAuth(CollectorInstanceUIState& ui, PacketCollector* collector, bool& needsRedraw) {
    if (!collector) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("SOCKS5认证（实例级）##socksConfig"))) {
        return;
    }

    ImGui::Indent();

    bool enableSocksAuth = collector->IsSocks5AuthEnabled();
    if (ImGuiGBK::Checkbox("启用SOCKS5认证##enableSocks", &enableSocksAuth)) {
        collector->SetSocks5Auth(enableSocksAuth);
        AB_LOG_INFO("[采集实例] SOCKS5认证已" + std::string(enableSocksAuth ? "启用" : "关闭"));
        needsRedraw = true;
    }

    const bool usingExternalAccounts = (collector->GetExternalAccountSource() != nullptr);
    if (usingExternalAccounts) {
        ImGuiGBK::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "账号源: 外部共享（当前实例认证使用外部账号）");
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "提示：下面的账号管理仅作用于本地账号表，不参与当前实例认证。");
    } else {
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "账号源: 本实例");
    }

    ImGui::Separator();

    ImGuiGBK::Text("账号管理");
    ImGui::Spacing();

    ImGuiGBK::Text("用户名:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##acctUser", ui.accountUsername, sizeof(ui.accountUsername));

    ImGuiGBK::Text("密码:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##acctPass", ui.accountPassword, sizeof(ui.accountPassword), ImGuiInputTextFlags_Password);

    ImGuiGBK::Text("到期时间:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##acctExpire", ui.accountExpireTime, sizeof(ui.accountExpireTime));

    ImGuiGBK::Text("最大连接:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("##acctMaxConn", &ui.accountMaxConnections);

    ImGuiGBK::Text("启用:");
    ImGui::SameLine();
    ImGui::Checkbox("##acctEnabled", &ui.accountEnabled);

    if (ImGuiGBK::Button("新增/更新账号##addOrUpdate", ImVec2(160, 0))) {
        const std::string username = ui.accountUsername;
        const std::string password = ui.accountPassword;
        const std::string expire = ui.accountExpireTime;
        const int maxConn = std::max(0, ui.accountMaxConnections);
        const bool isEnabled = ui.accountEnabled;

        if (username.empty()) {
            UiMessageBox::Show(g_mainHwnd, "用户名不能为空", "错误", MB_OK | MB_ICONERROR);
        } else if (usingExternalAccounts) {
            UiMessageBox::Show(g_mainHwnd, "当前实例使用外部账号源，请在账号源实例中管理账号。", "提示", MB_OK | MB_ICONINFORMATION);
        } else {
            if (!collector->AccountExists(username)) {
                if (collector->AddAccount(username, password, expire, maxConn)) {
                    AB_LOG_INFO("[采集实例] 新增账号成功: " + username);
                    needsRedraw = true;
                } else {
                    AB_LOG_ERROR("[采集实例] 新增账号失败: " + username);
                }
            } else {
                if (collector->UpdateAccount(username, password, expire, maxConn, isEnabled)) {
                    AB_LOG_INFO("[采集实例] 更新账号成功: " + username);
                    needsRedraw = true;
                } else {
                    AB_LOG_ERROR("[采集实例] 更新账号失败: " + username);
                }
            }
        }
    }

    ImGui::Separator();

    auto accounts = collector->GetAllAccounts();
    ImGuiGBK::Text("本地账号数: %d", static_cast<int>(accounts.size()));
    ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "提示：maxConnections=0 表示不限制连接数");

    if (ImGui::BeginTable("SocksAccountTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220))) {
        ImGui::TableSetupColumn("用户名", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("到期时间", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("最大", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("当前", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("最后IP", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (const auto& a : accounts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", a.username.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s", a.expireTime.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", a.maxConnections);
            ImGui::TableNextColumn();
            ImGui::Text("%d", a.currentConnections);
            ImGui::TableNextColumn();
            ImGui::Text("%s", a.isEnabled ? "是" : "否");
            ImGui::TableNextColumn();
            ImGui::Text("%s", a.lastLoginIP.c_str());
            ImGui::TableNextColumn();

            ImGui::PushID(a.username.c_str());
            if (ImGuiGBK::Button("删除##delAcct", ImVec2(60, 0))) {
                if (usingExternalAccounts) {
                    UiMessageBox::Show(g_mainHwnd, "当前实例使用外部账号源，请在账号源实例中删除账号。", "提示", MB_OK | MB_ICONINFORMATION);
                } else if (UiMessageBox::Show(g_mainHwnd, ("确定删除账号 " + a.username + " 吗？").c_str(), "确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    if (collector->RemoveAccount(a.username)) {
                        AB_LOG_INFO("[采集实例] 删除账号: " + a.username);
                        needsRedraw = true;
                    }
                }
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::Unindent();
}

static void CollectorUI_RenderSecondaryProxy(CollectorInstanceUIState& ui, PacketCollector* collector, bool& needsRedraw) {
    if (!collector) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("二级代理（实例级）##proxyConfig"))) {
        return;
    }

    ImGui::Indent();

    if (ImGuiGBK::Checkbox("启用二级代理##enableProxy", &ui.enableSecondaryProxy)) {
        needsRedraw = true;
    }

    ImGuiGBK::Text("代理地址:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##proxyHost", ui.proxyHost, sizeof(ui.proxyHost));

    ImGuiGBK::Text("代理端口:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("##proxyPort", &ui.proxyPort);

    ImGuiGBK::Text("用户名:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##proxyUser", ui.proxyUsername, sizeof(ui.proxyUsername));

    ImGuiGBK::Text("密码:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##proxyPass", ui.proxyPassword, sizeof(ui.proxyPassword), ImGuiInputTextFlags_Password);

    if (ImGuiGBK::Button("应用二级代理配置##applyProxy", ImVec2(160, 0))) {
        const int port = ui.proxyPort;
        if (ui.enableSecondaryProxy && (port <= 0 || port > 65535)) {
            UiMessageBox::Show(g_mainHwnd, "端口号无效（1-65535）", "错误", MB_OK | MB_ICONERROR);
        } else {
            collector->SetSecondaryProxy(ui.enableSecondaryProxy, ui.proxyHost, port, ui.proxyUsername, ui.proxyPassword);
            AB_LOG_INFO("[采集实例] 已应用二级代理配置");
            needsRedraw = true;
        }
    }

    ImGui::Unindent();
}
} // namespace

static void RenderCollectorInstanceConfig_Full(CollectorInstance* instance, bool& needsRedraw) {
    if (!instance) {
        ImGuiGBK::Text("实例不存在");
        return;
    }

    const InstanceInfo info = instance->GetInfo();
    PacketCollector* collector = instance->GetCollector();

    if (!collector) {
        ImGuiGBK::Text("采集器未初始化");
        return;
    }

    CollectorInstanceUIState& ui = CollectorInstUI_GetState(info.id);
    CollectorInstUI_LoadOnce(ui, collector);

    ImGui::PushID(info.id.c_str());

    CollectorUI_RenderInfoStats(info, collector);
    ImGui::Separator();
    CollectorUI_RenderStorageMode(ui, instance, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderPacketHeaderConfig(ui, instance, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderCollectorWhitelist(ui, instance, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderCollectorBlacklist(ui, instance, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderFilter(ui, collector, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderSocksAuth(ui, collector, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderSecondaryProxy(ui, collector, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderPerformance(ui, instance, collector, info, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderAntiCC(ui, collector, info, needsRedraw);
    ImGui::Separator();
    CollectorUI_RenderGameIdBindings(collector);
    ImGui::Separator();
    CollectorUI_RenderControls(instance, info, needsRedraw);

    ImGui::PopID();
}

namespace {
static void CollectorUI_RenderInfoStats(const InstanceInfo& info, PacketCollector* collector) {
    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "实例信息");
    ImGui::Separator();

    ImGuiGBK::Text("实例ID: %s", info.id.c_str());
    ImGuiGBK::Text("实例名称: %s", info.name.c_str());
    ImGuiGBK::Text("监听端口: %d", info.port);
    ImGuiGBK::Text("创建时间: %s", info.createTime.c_str());

    ImGuiGBK::Text("运行状态: ");
    ImGui::SameLine();
    if (info.state == InstanceState::Running) {
        ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "● 运行中");
    } else if (info.state == InstanceState::Starting) {
        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "● 启动中");
    } else if (info.state == InstanceState::Stopping) {
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "● 停止中");
    } else if (info.state == InstanceState::Error) {
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "● 错误");
    } else {
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "● 已停止");
    }

    if (!info.lastError.empty()) {
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "错误: %s", info.lastError.c_str());
    }

    ImGui::Separator();

    ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "统计信息");
    ImGui::Separator();

    ImGuiGBK::Text("当前连接数: %d", info.currentConnections);
    ImGuiGBK::Text("总数据包数: %llu", info.totalPackets);
    ImGuiGBK::Text("总流量: %.2f KB", info.totalBytes / 1024.0);

    if (collector) {
        ImGuiGBK::Text("分包数: %d", collector->GetFragmentedPackets());
        ImGuiGBK::Text("粘包数: %d", collector->GetMultiPackets());
        ImGuiGBK::Text("过滤连接数: %d", collector->GetFilteredConnections());
        ImGuiGBK::Text("回调队列: %zu", collector->GetCallbackQueueSize());
        ImGuiGBK::Text("数据库队列: %zu", collector->GetDbQueueSize());
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader(ImGuiText::U("WPE滤镜（全局）##wpeGlobal"))) {
        ImGui::Indent();
        ImGuiGBK::Text("提示: WPE滤镜是全局配置，会影响所有实例（采集/伪心跳/响应方向等）。");
        if (ImGuiGBK::Button("打开WPE滤镜页面##openWPE", ImVec2(160, 0))) {
            AB_LOG_INFO("[实例配置] 请在WPE滤镜页面配置滤镜");
        }
        ImGui::Unindent();
    }
}

static void CollectorUI_RenderStorageMode(CollectorInstanceUIState& ui, CollectorInstance* instance, bool& needsRedraw) {
    if (!instance) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("存储模式与ID过滤##storageModeConfig"))) {
        return;
    }

    ImGui::Indent();

    // 加载当前配置
    if (!ui.storageModeLoaded) {
        ui.storageMode = static_cast<int>(instance->GetStorageMode());
        ui.allowCollect00ID = instance->GetAllowCollect00ID();
        ui.allowCollectOBID = instance->GetAllowCollectOBID();
        ui.storageModeLoaded = true;
    }

    // ===== 存储模式 =====
    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "存储模式");
    ImGui::Separator();

    const char* storageModes[] = { "数据库存储", "内存存储" };
    ImGuiGBK::Text("存储模式:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("##storageMode", &ui.storageMode, storageModes, 2)) {
        instance->SetStorageMode(static_cast<InstMgr::StorageMode>(ui.storageMode));
        AB_LOG_INFO(std::string("[采集实例] 存储模式已切换为: ") + storageModes[ui.storageMode]);
        needsRedraw = true;
    }

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGuiGBK::Text("数据库存储: 数据保存到数据库，持久化存储");
        ImGuiGBK::Text("内存存储: 数据保存到内存池，速度快但重启后丢失");
        ImGui::EndTooltip();
    }

    // 显示当前内存池状态（单伪采集：内存存储由 DatabaseManager::memoryRecords 承载）
    if (ui.storageMode == 1) {  // 内存存储模式
        DatabaseManager* db = instance->GetDatabase();
        if (db) {
            const auto records = db->GetMemoryRecords(15000);
            std::set<std::string> usernames;
            std::set<std::string> gameIDs;
            for (const auto& r : records) {
                if (!r.socksUsername.empty()) usernames.insert(r.socksUsername);
                if (!r.gameID.empty()) gameIDs.insert(r.gameID);
            }

            ImGuiGBK::Text("内存池数据: %d 条", static_cast<int>(records.size()));
            ImGuiGBK::Text("用户名数量: %d", static_cast<int>(usernames.size()));
            ImGuiGBK::Text("游戏ID数量: %d", static_cast<int>(gameIDs.size()));

            if (ImGuiGBK::Button("清空内存池##clearMemPool", ImVec2(120, 0))) {
                db->ClearMemoryRecords();
                AB_LOG_INFO("[采集实例] 内存池已清空");
                needsRedraw = true;
            }
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ===== 特殊ID过滤 =====
    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "特殊ID过滤");
    ImGui::Separator();

    ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "控制是否采集特殊格式的GameID");
    ImGui::Spacing();

    if (ImGuiGBK::Checkbox("采集00开头的GameID##allow00ID", &ui.allowCollect00ID)) {
        instance->SetAllowCollect00ID(ui.allowCollect00ID);
        AB_LOG_INFO(std::string("[采集实例] 00开头GameID采集: ") + (ui.allowCollect00ID ? "启用" : "禁用"));
        needsRedraw = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGuiGBK::Text("00开头的GameID通常是特殊账号或测试账号");
        ImGuiGBK::Text("默认不采集这类ID");
        ImGui::EndTooltip();
    }

    if (ImGuiGBK::Checkbox("采集_OB结尾的GameID##allowOBID", &ui.allowCollectOBID)) {
        instance->SetAllowCollectOBID(ui.allowCollectOBID);
        AB_LOG_INFO(std::string("[采集实例] _OB结尾GameID采集: ") + (ui.allowCollectOBID ? "启用" : "禁用"));
        needsRedraw = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGuiGBK::Text("_OB结尾的GameID通常是观战账号");
        ImGuiGBK::Text("默认不采集这类ID");
        ImGui::EndTooltip();
    }

    ImGui::Unindent();
}

static void CollectorUI_RenderPacketHeaderConfig(CollectorInstanceUIState& ui, CollectorInstance* instance, bool& needsRedraw) {
    if (!instance) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("包头类型 / 62特征（对齐独立项目）##packetHeaderCfg"))) {
        return;
    }

    ImGui::Indent();

    if (!ui.headerLoaded) {
        ui.disablePacketHeaderFilter = instance->GetDisablePacketHeaderFilter();
        ui.enableCollect62Pattern = instance->GetEnableCollect62Pattern();

        const auto types = instance->GetCollectorPacketTypes();
        std::string csv;
        for (size_t i = 0; i < types.size(); i++) {
            if (i) csv += ",";
            csv += std::to_string(types[i]);
        }
        std::snprintf(ui.collectorPacketTypesBuf, sizeof(ui.collectorPacketTypesBuf), "%s", csv.empty() ? "1,2,3,4" : csv.c_str());
        ui.headerLoaded = true;
    }

    if (ImGuiGBK::Checkbox("全伪模式（忽略包头限制）##disableHeaderFilter", &ui.disablePacketHeaderFilter)) {
        instance->SetDisablePacketHeaderFilter(ui.disablePacketHeaderFilter);
        AB_LOG_INFO(std::string("[采集实例] 包头过滤已") + (ui.disablePacketHeaderFilter ? "关闭(全伪)" : "开启"));
        needsRedraw = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGuiGBK::Text("启用后将忽略“包头类型启用列表”，所有类型均可采集（仍受白/黑名单与特殊ID过滤影响）。");
        ImGui::EndTooltip();
    }

    if (ImGuiGBK::Checkbox("启用62特征采集##enable62", &ui.enableCollect62Pattern)) {
        instance->SetEnableCollect62Pattern(ui.enableCollect62Pattern);
        AB_LOG_INFO(std::string("[采集实例] 62特征采集已") + (ui.enableCollect62Pattern ? "启用" : "关闭"));
        needsRedraw = true;
    }

    ImGui::Spacing();
    ImGuiGBK::Text("包头类型列表（逗号分隔）:");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("##collectorPacketTypesCsv", ui.collectorPacketTypesBuf, sizeof(ui.collectorPacketTypesBuf));
    ImGui::SameLine();
    if (ImGuiGBK::Button("应用##applyCollectorTypes", ImVec2(80, 0))) {
        std::vector<int> parsed;
        {
            std::stringstream ss(ui.collectorPacketTypesBuf);
            std::string item;
            while (std::getline(ss, item, ',')) {
                try {
                    int t = std::stoi(item);
                    if (t >= 0 && t <= 255) parsed.push_back(t);
                } catch (...) {}
            }
        }
        instance->SetCollectorPacketTypes(parsed);

        // 回写一份标准化CSV到输入框（便于对齐显示）
        const auto types = instance->GetCollectorPacketTypes();
        std::string csv;
        for (size_t i = 0; i < types.size(); i++) {
            if (i) csv += ",";
            csv += std::to_string(types[i]);
        }
        std::snprintf(ui.collectorPacketTypesBuf, sizeof(ui.collectorPacketTypesBuf), "%s", csv.empty() ? "1,2,3,4" : csv.c_str());

        AB_LOG_INFO("[采集实例] 已应用包头类型列表");
        needsRedraw = true;
    }

    const auto types = instance->GetCollectorPacketTypes();
    if (!types.empty()) {
        if (ImGuiGBK::Button("全选##typesAll", ImVec2(80, 0))) {
            for (int t : types) instance->SetCollectorPacketTypeEnabled(t, true);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGuiGBK::Button("全不选##typesNone", ImVec2(80, 0))) {
            for (int t : types) instance->SetCollectorPacketTypeEnabled(t, false);
            needsRedraw = true;
        }

        if (ImGui::BeginTable("CollectorTypeEnabledTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, 220))) {
            ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int t : types) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", t);
                ImGui::TableNextColumn();

                bool enabled = instance->GetCollectorPacketTypeEnabled(t);
                std::string chk = "##typeEnable_" + std::to_string(t);
                if (ImGui::Checkbox(chk.c_str(), &enabled)) {
                    instance->SetCollectorPacketTypeEnabled(t, enabled);
                    needsRedraw = true;
                }

                ImGui::TableNextColumn();
                const std::string desc = PacketParser::GetPacketTypeDesc(t);
                ImGui::Text("%s", desc.c_str());
            }

            ImGui::EndTable();
        }
    }

    ImGui::Unindent();
}

static void CollectorUI_RenderCollectorWhitelist(CollectorInstanceUIState& ui, CollectorInstance* instance, bool& needsRedraw) {
    if (!instance) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("采集白名单##collectorWhitelist"))) {
        return;
    }

    ImGui::Indent();

    // 加载当前配置
    if (!ui.collectorWhitelistLoaded) {
        ui.enableCollectorWhitelist = instance->GetEnableCollectorWhitelist();
        ui.collectorWhitelistLoaded = true;
    }

    // 启用开关
    if (ImGuiGBK::Checkbox("启用采集白名单##enableCollectorWL", &ui.enableCollectorWhitelist)) {
        instance->SetEnableCollectorWhitelist(ui.enableCollectorWhitelist);
        AB_LOG_INFO(std::string("[采集白名单] ") + (ui.enableCollectorWhitelist ? "已启用" : "已禁用"));
        needsRedraw = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGuiGBK::Text("启用后，只有匹配白名单规则的数据包才会被采集");
        ImGuiGBK::Text("（写入数据库或内存池）");
        ImGui::EndTooltip();
    }

    ImGui::Spacing();

    // 添加规则按钮
    if (ImGuiGBK::Button("添加规则##addCollectorWLRule", ImVec2(100, 0))) {
        ui.editingWhitelistRuleId = -1;
        std::memset(ui.whitelistRuleName, 0, sizeof(ui.whitelistRuleName));
        std::memset(ui.whitelistRulePattern, 0, sizeof(ui.whitelistRulePattern));
        ui.whitelistRuleUsePatternSearch = true;
        ui.whitelistRuleEnabled = true;
        ui.showWhitelistEditor = true;
    }

    ImGui::SameLine();
    if (ImGuiGBK::Button("清空规则##clearCollectorWLRules", ImVec2(100, 0))) {
        instance->ClearCollectorWhitelistRules();
        needsRedraw = true;
    }

    ImGui::Spacing();

    // 规则列表
    auto rules = instance->GetCollectorWhitelistRules();
    if (!rules.empty()) {
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "规则列表 (%d条)", static_cast<int>(rules.size()));

        if (ImGui::BeginTable("##collectorWLRulesTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(ImGuiText::U("ID"), ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn(ImGuiText::U("名称"), ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn(ImGuiText::U("模式"), ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn(ImGuiText::U("匹配模式"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(ImGuiText::U("命中"), ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn(ImGuiText::U("操作"), ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableHeadersRow();

            for (const auto& rule : rules) {
                ImGui::TableNextRow();

                // ID
                ImGui::TableNextColumn();
                ImGui::Text("%d", rule.id);

                // 名称
                ImGui::TableNextColumn();
                if (rule.isEnabled) {
                    ImGui::Text("%s", rule.name.c_str());
                } else {
                    ImGui::TextDisabled("%s", rule.name.c_str());
                }

                // 搜索模式
                ImGui::TableNextColumn();
                ImGuiGBK::Text(rule.usePatternSearch ? "位置匹配" : "包含匹配");

                // 匹配模式
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", rule.searchPattern.c_str());

                // 命中次数
                ImGui::TableNextColumn();
                ImGui::Text("%llu", rule.matchCount);

                // 操作
                ImGui::TableNextColumn();
                ImGui::PushID(rule.id);

                if (ImGuiGBK::SmallButton("编辑")) {
                    ui.editingWhitelistRuleId = rule.id;
                    std::snprintf(ui.whitelistRuleName, sizeof(ui.whitelistRuleName), "%s", rule.name.c_str());
                    std::snprintf(ui.whitelistRulePattern, sizeof(ui.whitelistRulePattern), "%s", rule.searchPattern.c_str());
                    ui.whitelistRuleUsePatternSearch = rule.usePatternSearch;
                    ui.whitelistRuleEnabled = rule.isEnabled;
                    ui.showWhitelistEditor = true;
                }
                ImGui::SameLine();
                if (ImGuiGBK::SmallButton("删除")) {
                    instance->RemoveCollectorWhitelistRule(rule.id);
                    needsRedraw = true;
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }
    } else {
        ImGuiGBK::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "暂无规则");
    }

    // 编辑弹窗
    if (ui.showWhitelistEditor) {
        ImGui::OpenPopup(ImGuiText::U("编辑采集白名单规则##editCollectorWLRule"));
    }

    if (ImGui::BeginPopupModal(ImGuiText::U("编辑采集白名单规则##editCollectorWLRule"), &ui.showWhitelistEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGuiGBK::Text("规则名称:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##wlRuleName", ui.whitelistRuleName, sizeof(ui.whitelistRuleName));

        ImGui::Spacing();

        ImGuiGBK::Text("搜索模式:");
        if (ImGuiGBK::RadioButton("位置匹配##wlPatternSearch", ui.whitelistRuleUsePatternSearch)) {
            ui.whitelistRuleUsePatternSearch = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("格式: pos|hex,pos|hex,...");
            ImGuiGBK::Text("例如: 0|01020304,10|AABBCCDD");
            ImGuiGBK::Text("表示位置0处匹配01020304，位置10处匹配AABBCCDD");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        if (ImGuiGBK::RadioButton("包含匹配##wlContainSearch", !ui.whitelistRuleUsePatternSearch)) {
            ui.whitelistRuleUsePatternSearch = false;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("在数据包任意位置搜索指定的HEX字节");
            ImGuiGBK::Text("例如: 01020304");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();

        ImGuiGBK::Text("匹配模式:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##wlRulePattern", ui.whitelistRulePattern, sizeof(ui.whitelistRulePattern));

        ImGui::Spacing();

        ImGuiGBK::Checkbox("启用##wlRuleEnabled", &ui.whitelistRuleEnabled);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGuiGBK::Button("保存##saveWLRule", ImVec2(100, 0))) {
            InstMgr::CollectorWhitelistRule rule;
            rule.name = ui.whitelistRuleName;
            rule.searchPattern = ui.whitelistRulePattern;
            rule.usePatternSearch = ui.whitelistRuleUsePatternSearch;
            rule.isEnabled = ui.whitelistRuleEnabled;

            if (ui.editingWhitelistRuleId > 0) {
                instance->UpdateCollectorWhitelistRule(ui.editingWhitelistRuleId, rule);
            } else {
                instance->AddCollectorWhitelistRule(rule);
            }

            ui.showWhitelistEditor = false;
            needsRedraw = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGuiGBK::Button("取消##cancelWLRule", ImVec2(100, 0))) {
            ui.showWhitelistEditor = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::Unindent();
}

static void CollectorUI_RenderCollectorBlacklist(CollectorInstanceUIState& ui, CollectorInstance* instance, bool& needsRedraw) {
    if (!instance) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("采集黑名单##collectorBlacklist"))) {
        return;
    }

    ImGui::Indent();

    // 加载当前配置
    if (!ui.collectorBlacklistLoaded) {
        ui.enableCollectorBlacklist = instance->GetEnableCollectorBlacklist();
        ui.collectorBlacklistLoaded = true;
    }

    // 启用开关
    if (ImGuiGBK::Checkbox("启用采集黑名单##enableCollectorBL", &ui.enableCollectorBlacklist)) {
        instance->SetEnableCollectorBlacklist(ui.enableCollectorBlacklist);
        AB_LOG_INFO(std::string("[采集黑名单] ") + (ui.enableCollectorBlacklist ? "已启用" : "已禁用"));
        needsRedraw = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGuiGBK::Text("启用后，匹配黑名单规则的数据包将不会被采集");
        ImGuiGBK::Text("（不写入数据库或内存池）");
        ImGui::EndTooltip();
    }

    ImGui::Spacing();

    // 添加规则按钮
    if (ImGuiGBK::Button("添加规则##addCollectorBLRule", ImVec2(100, 0))) {
        ui.editingBlacklistRuleId = -1;
        std::memset(ui.blacklistRuleName, 0, sizeof(ui.blacklistRuleName));
        std::memset(ui.blacklistRulePattern, 0, sizeof(ui.blacklistRulePattern));
        ui.blacklistRuleUsePatternSearch = true;
        ui.blacklistRuleEnabled = true;
        ui.showBlacklistEditor = true;
    }

    ImGui::SameLine();
    if (ImGuiGBK::Button("清空规则##clearCollectorBLRules", ImVec2(100, 0))) {
        instance->ClearCollectorBlacklistRules();
        needsRedraw = true;
    }

    ImGui::Spacing();

    // 规则列表
    auto rules = instance->GetCollectorBlacklistRules();
    if (!rules.empty()) {
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "规则列表 (%d条)", static_cast<int>(rules.size()));

        if (ImGui::BeginTable("##collectorBLRulesTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(ImGuiText::U("ID"), ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn(ImGuiText::U("名称"), ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn(ImGuiText::U("模式"), ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn(ImGuiText::U("匹配模式"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(ImGuiText::U("命中"), ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn(ImGuiText::U("操作"), ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableHeadersRow();

            for (const auto& rule : rules) {
                ImGui::TableNextRow();

                // ID
                ImGui::TableNextColumn();
                ImGui::Text("%d", rule.id);

                // 名称
                ImGui::TableNextColumn();
                if (rule.isEnabled) {
                    ImGui::Text("%s", rule.name.c_str());
                } else {
                    ImGui::TextDisabled("%s", rule.name.c_str());
                }

                // 搜索模式
                ImGui::TableNextColumn();
                ImGuiGBK::Text(rule.usePatternSearch ? "位置匹配" : "包含匹配");

                // 匹配模式
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", rule.searchPattern.c_str());

                // 命中次数
                ImGui::TableNextColumn();
                ImGui::Text("%llu", rule.matchCount);

                // 操作
                ImGui::TableNextColumn();
                ImGui::PushID(rule.id + 10000);  // 避免与白名单ID冲突

                if (ImGuiGBK::SmallButton("编辑")) {
                    ui.editingBlacklistRuleId = rule.id;
                    std::snprintf(ui.blacklistRuleName, sizeof(ui.blacklistRuleName), "%s", rule.name.c_str());
                    std::snprintf(ui.blacklistRulePattern, sizeof(ui.blacklistRulePattern), "%s", rule.searchPattern.c_str());
                    ui.blacklistRuleUsePatternSearch = rule.usePatternSearch;
                    ui.blacklistRuleEnabled = rule.isEnabled;
                    ui.showBlacklistEditor = true;
                }
                ImGui::SameLine();
                if (ImGuiGBK::SmallButton("删除")) {
                    instance->RemoveCollectorBlacklistRule(rule.id);
                    needsRedraw = true;
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }
    } else {
        ImGuiGBK::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "暂无规则");
    }

    // 编辑弹窗
    if (ui.showBlacklistEditor) {
        ImGui::OpenPopup(ImGuiText::U("编辑采集黑名单规则##editCollectorBLRule"));
    }

    if (ImGui::BeginPopupModal(ImGuiText::U("编辑采集黑名单规则##editCollectorBLRule"), &ui.showBlacklistEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGuiGBK::Text("规则名称:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##blRuleName", ui.blacklistRuleName, sizeof(ui.blacklistRuleName));

        ImGui::Spacing();

        ImGuiGBK::Text("搜索模式:");
        if (ImGuiGBK::RadioButton("位置匹配##blPatternSearch", ui.blacklistRuleUsePatternSearch)) {
            ui.blacklistRuleUsePatternSearch = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("格式: pos|hex,pos|hex,...");
            ImGuiGBK::Text("例如: 0|01020304,10|AABBCCDD");
            ImGuiGBK::Text("表示位置0处匹配01020304，位置10处匹配AABBCCDD");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        if (ImGuiGBK::RadioButton("包含匹配##blContainSearch", !ui.blacklistRuleUsePatternSearch)) {
            ui.blacklistRuleUsePatternSearch = false;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("在数据包任意位置搜索指定的HEX字节");
            ImGuiGBK::Text("例如: 01020304");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();

        ImGuiGBK::Text("匹配模式:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##blRulePattern", ui.blacklistRulePattern, sizeof(ui.blacklistRulePattern));

        ImGui::Spacing();

        ImGuiGBK::Checkbox("启用##blRuleEnabled", &ui.blacklistRuleEnabled);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGuiGBK::Button("保存##saveBLRule", ImVec2(100, 0))) {
            InstMgr::CollectorBlacklistRule rule;
            rule.name = ui.blacklistRuleName;
            rule.searchPattern = ui.blacklistRulePattern;
            rule.usePatternSearch = ui.blacklistRuleUsePatternSearch;
            rule.isEnabled = ui.blacklistRuleEnabled;

            if (ui.editingBlacklistRuleId > 0) {
                instance->UpdateCollectorBlacklistRule(ui.editingBlacklistRuleId, rule);
            } else {
                instance->AddCollectorBlacklistRule(rule);
            }

            ui.showBlacklistEditor = false;
            needsRedraw = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGuiGBK::Button("取消##cancelBLRule", ImVec2(100, 0))) {
            ui.showBlacklistEditor = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::Unindent();
}

static void CollectorUI_RenderFilter(CollectorInstanceUIState& ui, PacketCollector* collector, bool& needsRedraw) {
    if (!collector) return;

    if (!ImGui::CollapsingHeader(ImGuiText::U("过滤配置（连接级）##filterConfig"))) {
        return;
    }

    ImGui::Indent();

    const char* filterTypes[] = { "无", "IP", "域名", "端口" };
    ImGuiGBK::Text("过滤类型:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("##filterType", &ui.filterType, filterTypes, 4);

    ImGuiGBK::Text("过滤值:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("##filterValue", ui.filterValue, sizeof(ui.filterValue));

    if (ImGuiGBK::Button("应用过滤##applyFilter", ImVec2(120, 0))) {
        const FilterType ft = static_cast<FilterType>(ui.filterType);
        if (ft == FILTER_NONE || std::strlen(ui.filterValue) == 0) {
            collector->ClearFilter();
            ui.filterType = static_cast<int>(FILTER_NONE);
            ui.filterValue[0] = '\0';
            AB_LOG_INFO("[采集实例] 已清除过滤条件");
        } else {
            collector->SetFilter(ft, ui.filterValue);
            AB_LOG_INFO("[采集实例] 已应用过滤条件");
        }
        needsRedraw = true;
    }
    ImGui::SameLine();
    if (ImGuiGBK::Button("清除过滤##clearFilter", ImVec2(120, 0))) {
        collector->ClearFilter();
        ui.filterType = static_cast<int>(FILTER_NONE);
        ui.filterValue[0] = '\0';
        AB_LOG_INFO("[采集实例] 已清除过滤条件");
        needsRedraw = true;
    }

    ImGui::Unindent();
}

static void CollectorUI_RenderControls(CollectorInstance* instance, const InstanceInfo& info, bool& needsRedraw) {
    if (!instance) return;

    ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "操作");
    ImGui::Separator();

    if (info.state == InstanceState::Running) {
        if (ImGuiGBK::Button("停止实例##stopInst", ImVec2(120, 0))) {
            instance->Stop();
            AB_LOG_INFO("[实例配置] 实例已停止: " + info.id);
            needsRedraw = true;
        }
    } else {
        if (ImGuiGBK::Button("启动实例##startInst", ImVec2(120, 0))) {
            instance->Start();
            AB_LOG_INFO("[实例配置] 实例已启动: " + info.id);
            needsRedraw = true;
        }
    }

    ImGui::SameLine();
    if (ImGuiGBK::Button("刷新数据##refreshData", ImVec2(120, 0))) {
        needsRedraw = true;
    }
}
} // namespace


#endif

