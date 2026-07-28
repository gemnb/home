// T3Config.h
#pragma once

#include <string>

namespace T3Config {
    // t3网络验证配置
    static const std::string Host = "http://w.t3yanzheng.com"; // t3网络验证线路地址
    static const std::string AppKey = "08bca5060a674e520ca8c584596b7a23"; // t3网络验证程序应用密钥
    static const std::string Base64key = "IK7uAhVYMyL3/n6ckDtpoTsgrvOq5w+QPd8WFClN9UiRbSH4ZEz1JBGxfmejXa02"; // t3网络验证程序Base64自定义编码集

    // API路径
    static const std::string Path_SingleLogin = "B625AB3A24AF3179"; // t3网络验证程序单码卡密登录API调用路径
    static const std::string Path_IsSingleLoginStatus = "555D95448ED7D3CE"; // t3网络验证程序单码卡密登录状态查询API调用路径
    static const std::string Path_GetProgramNotice = "31C4254A040ABDA1"; // t3网络验证程序获取公告API调用路径
    static const std::string Path_GetProgramVersionNumber = "3E1111E6D9135FAF"; // t3网络验证程序获取版本号API调用路径
    static const std::string Path_GetValueContent = "5FBC608AFF3B5402"; // t3网络验证程序获取变量内容API调用路径
}