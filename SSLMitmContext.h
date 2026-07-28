#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sspi.h>
#include <schannel.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

// SSL MITM 规则配置
struct SSLMitmRule {
    bool matchByPort;        // true=按端口匹配, false=按域名匹配
    int port;                // 匹配端口（matchByPort=true 时有效）
    std::string domain;      // 匹配域名（matchByPort=false 时有效，支持前缀 * 通配）

    SSLMitmRule() : matchByPort(true), port(443) {}
    static SSLMitmRule ByPort(int p) { SSLMitmRule r; r.matchByPort = true; r.port = p; return r; }
    static SSLMitmRule ByDomain(const std::string& d) { SSLMitmRule r; r.matchByPort = false; r.domain = d; return r; }
};

// SChannel 单端上下文
struct SchannelSide {
    CtxtHandle  hCtx   = {};
    CredHandle  hCred  = {};
    bool        ctxOk  = false;
    bool        credOk = false;
    PCCERT_CONTEXT ownedCert = nullptr; // 服务端证书，持有私钥上下文直到连接释放
    std::vector<uint8_t> recvBuf;  // 底层加密数据缓冲（未解密）
    std::vector<uint8_t> plainBuf; // 解密后待消费数据

    // StreamSizes（握手后填充）
    SecPkgContext_StreamSizes streamSizes = {};
    bool streamSizesOk = false;
};

class SSLMitmContext {
public:
    SSLMitmContext() = default;
    ~SSLMitmContext() { Shutdown(); }

    // 对真实服务器建立 TLS 客户端，再对客户端建立 TLS 服务端
    // targetHost 用于服务器端 SNI + 生成假证书 CN
    bool Handshake(SOCKET clientSock, SOCKET serverSock, const std::string& targetHost);

    // 从客户端读明文（替代 recv）
    int ReadFromClient(std::vector<uint8_t>& outPlain);
    // 向客户端写明文（替代 send）
    bool WriteToClient(const uint8_t* data, int len);

    // 从服务器读明文（替代 recv）
    int ReadFromServer(std::vector<uint8_t>& outPlain);
    // 向服务器写明文（替代 send）
    bool WriteToServer(const uint8_t* data, int len);

    void Shutdown();

    // 导出 CA 证书（DER 格式），供用户手动安装信任
    static bool ExportCACert(const std::string& filePath);

    // 全局 CA 初始化（进程级，只调用一次）
    static bool InitGlobalCA();
    static std::string GetLastErrorDetail();

private:
    SOCKET clientSock_ = INVALID_SOCKET;
    SOCKET serverSock_ = INVALID_SOCKET;
    SchannelSide client_; // 对客户端扮演服务端
    SchannelSide server_; // 对服务器扮演客户端
    std::string targetHost_;

    // 对真实服务器做 TLS 客户端握手
    bool HandshakeAsClient(SOCKET sock, const std::string& host, SchannelSide& side);
    // 对客户端做 TLS 服务端握手（使用动态证书）
    bool HandshakeAsServer(SOCKET sock, const std::string& cn, SchannelSide& side);

    // 为域名动态签发证书（由全局 CA 签名，内存中）
    static PCCERT_CONTEXT IssueCert(const std::string& cn);

    // SChannel 底层加密发送
    bool SchannelEncryptSend(SOCKET sock, SchannelSide& side, const uint8_t* plain, int len);
    // SChannel 底层接收解密（追加到 side.plainBuf）
    // 返回 -1=错误/连接断开, 0=暂无数据, >0=解密字节数
    int SchannelRecvDecrypt(SOCKET sock, SchannelSide& side);

    // 全局 CA 证书（进程生命周期内有效）
    static PCCERT_CONTEXT s_caCert;
    static HCRYPTPROV     s_hProv;
    static HCRYPTKEY      s_hCaKey;
    static std::mutex     s_caMutex;
    static bool           s_caInited;

    void FreeSide(SchannelSide& side);
};
