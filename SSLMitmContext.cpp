#include "SSLMitmContext.h"
#include "Logger.h"
#include <array>
#include <iomanip>
#include <vector>
#include <string>
#include <sstream>

namespace {
constexpr DWORD kAsnEncoding = X509_ASN_ENCODING;
constexpr DWORD kRsa2048Flags = (2048 << 16) | CRYPT_EXPORTABLE;
constexpr wchar_t kCaKeyContainerName[] = L"AB3_SSLMITM_CA_CONTAINER";
std::mutex g_sslMitmLastErrorMutex;
std::string g_sslMitmLastErrorDetail;

void SetLastSslMitmErrorDetail(const std::string& detail) {
    std::lock_guard<std::mutex> lock(g_sslMitmLastErrorMutex);
    g_sslMitmLastErrorDetail = detail;
}

std::string GetLastSslMitmErrorDetailCopy() {
    std::lock_guard<std::mutex> lock(g_sslMitmLastErrorMutex);
    return g_sslMitmLastErrorDetail;
}

std::string TrimTrailingWhitespace(std::string text) {
    while (!text.empty()) {
        const unsigned char ch = static_cast<unsigned char>(text.back());
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            text.pop_back();
        } else {
            break;
        }
    }
    return text;
}

std::string FormatWin32ErrorMessage(DWORD error) {
    LPSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(
        flags,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);

    std::string message;
    if (length > 0 && buffer) {
        message.assign(buffer, length);
        LocalFree(buffer);
        message = TrimTrailingWhitespace(message);
    }

    if (message.empty()) {
        message = "Unknown error";
    }
    return message;
}

void LogCryptoFailure(const std::string& step, DWORD error = GetLastError()) {
    std::ostringstream oss;
    oss << "[SSL-MITM] " << step
        << " failed (code=" << error
        << ", hex=0x" << std::hex << std::uppercase << error << std::dec << ")"
        << ": " << FormatWin32ErrorMessage(error);
    SetLastSslMitmErrorDetail(oss.str());
    AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, oss.str());
}

struct AutoLocalBuffer {
    BYTE* data = nullptr;
    DWORD size = 0;

    AutoLocalBuffer() = default;
    AutoLocalBuffer(const AutoLocalBuffer&) = delete;
    AutoLocalBuffer& operator=(const AutoLocalBuffer&) = delete;

    ~AutoLocalBuffer() {
        if (data) {
            LocalFree(data);
        }
    }

    CRYPT_DATA_BLOB AsBlob() const {
        CRYPT_DATA_BLOB blob = {};
        blob.cbData = size;
        blob.pbData = data;
        return blob;
    }
};

bool EncodeObject(LPCSTR structType, const void* info, AutoLocalBuffer& out) {
    DWORD encodedSize = 0;
    BYTE* encodedData = nullptr;
    if (!CryptEncodeObjectEx(kAsnEncoding, structType, info,
                             CRYPT_ENCODE_ALLOC_FLAG, nullptr,
                             reinterpret_cast<void*>(&encodedData), &encodedSize)) {
        LogCryptoFailure("CryptEncodeObjectEx");
        return false;
    }

    out.data = encodedData;
    out.size = encodedSize;
    return true;
}

bool BuildSubjectNameBlob(const std::string& commonName, AutoLocalBuffer& out) {
    const std::string subject = "CN=" + commonName;

    DWORD encodedSize = 0;
    if (!CertStrToNameA(kAsnEncoding, subject.c_str(), CERT_X500_NAME_STR,
                        nullptr, nullptr, &encodedSize, nullptr)) {
        LogCryptoFailure("CertStrToNameA(size)");
        return false;
    }

    BYTE* encodedData = static_cast<BYTE*>(LocalAlloc(LMEM_FIXED, encodedSize));
    if (!encodedData) {
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[SSL-MITM] LocalAlloc failed while building subject name");
        return false;
    }

    if (!CertStrToNameA(kAsnEncoding, subject.c_str(), CERT_X500_NAME_STR,
                        nullptr, encodedData, &encodedSize, nullptr)) {
        LogCryptoFailure("CertStrToNameA(encode)");
        LocalFree(encodedData);
        return false;
    }

    out.data = encodedData;
    out.size = encodedSize;
    return true;
}

bool ExportPublicKeyInfo(HCRYPTPROV hProv, DWORD keySpec,
                         AutoLocalBuffer& out,
                         PCERT_PUBLIC_KEY_INFO& publicKeyInfo) {
    DWORD encodedSize = 0;
    if (!CryptExportPublicKeyInfo(hProv, keySpec, kAsnEncoding, nullptr, &encodedSize)) {
        LogCryptoFailure("CryptExportPublicKeyInfo(size)");
        return false;
    }

    BYTE* encodedData = static_cast<BYTE*>(LocalAlloc(LMEM_FIXED, encodedSize));
    if (!encodedData) {
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[SSL-MITM] LocalAlloc failed while exporting public key info");
        return false;
    }

    auto* keyInfo = reinterpret_cast<PCERT_PUBLIC_KEY_INFO>(encodedData);
    if (!CryptExportPublicKeyInfo(hProv, keySpec, kAsnEncoding, keyInfo, &encodedSize)) {
        LogCryptoFailure("CryptExportPublicKeyInfo(export)");
        LocalFree(encodedData);
        return false;
    }

    out.data = encodedData;
    out.size = encodedSize;
    publicKeyInfo = keyInfo;
    return true;
}

bool CreateSerialNumber(HCRYPTPROV hProv,
                        std::array<BYTE, 16>& serialBytes,
                        CRYPT_INTEGER_BLOB& serial) {
    if (!CryptGenRandom(hProv, static_cast<DWORD>(serialBytes.size()), serialBytes.data())) {
        LogCryptoFailure("CryptGenRandom(serial)");
        return false;
    }

    serialBytes.back() &= 0x7F;
    bool allZero = true;
    for (BYTE b : serialBytes) {
        if (b != 0) {
            allZero = false;
            break;
        }
    }
    if (allZero) {
        serialBytes[0] = 1;
    }

    serial.cbData = static_cast<DWORD>(serialBytes.size());
    serial.pbData = serialBytes.data();
    return true;
}

bool BuildBasicConstraintsExtension(bool isCa,
                                    CERT_EXTENSION& extension,
                                    AutoLocalBuffer& encoded) {
    CERT_BASIC_CONSTRAINTS2_INFO constraints = {};
    constraints.fCA = isCa ? TRUE : FALSE;
    constraints.fPathLenConstraint = FALSE;
    constraints.dwPathLenConstraint = 0;

    if (!EncodeObject(X509_BASIC_CONSTRAINTS2, &constraints, encoded)) {
        return false;
    }

    extension.pszObjId = const_cast<LPSTR>(szOID_BASIC_CONSTRAINTS2);
    extension.fCritical = TRUE;
    extension.Value = encoded.AsBlob();
    return true;
}

bool BuildKeyUsageExtension(BYTE usageFlags,
                            CERT_EXTENSION& extension,
                            AutoLocalBuffer& encoded) {
    BYTE usage = usageFlags;
    CRYPT_BIT_BLOB keyUsage = {};
    keyUsage.cbData = 1;
    keyUsage.pbData = &usage;
    keyUsage.cUnusedBits = 0;

    if (!EncodeObject(X509_KEY_USAGE, &keyUsage, encoded)) {
        return false;
    }

    extension.pszObjId = const_cast<LPSTR>(szOID_KEY_USAGE);
    extension.fCritical = TRUE;
    extension.Value = encoded.AsBlob();
    return true;
}

bool BuildEnhancedKeyUsageExtension(CERT_EXTENSION& extension,
                                    AutoLocalBuffer& encoded) {
    LPSTR usageIdentifiers[1] = {
        const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)
    };
    CERT_ENHKEY_USAGE usage = {};
    usage.cUsageIdentifier = 1;
    usage.rgpszUsageIdentifier = usageIdentifiers;

    if (!EncodeObject(X509_ENHANCED_KEY_USAGE, &usage, encoded)) {
        return false;
    }

    extension.pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
    extension.fCritical = FALSE;
    extension.Value = encoded.AsBlob();
    return true;
}

bool BuildSubjectAltNameExtension(const std::string& host,
                                  CERT_EXTENSION& extension,
                                  AutoLocalBuffer& encoded) {
    CERT_ALT_NAME_ENTRY altName = {};
    IN_ADDR ipv4 = {};
    IN6_ADDR ipv6 = {};
    std::wstring wideHost;

    if (InetPtonA(AF_INET, host.c_str(), &ipv4) == 1) {
        altName.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        altName.IPAddress.cbData = sizeof(ipv4);
        altName.IPAddress.pbData = reinterpret_cast<BYTE*>(&ipv4);
    } else if (InetPtonA(AF_INET6, host.c_str(), &ipv6) == 1) {
        altName.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        altName.IPAddress.cbData = sizeof(ipv6);
        altName.IPAddress.pbData = reinterpret_cast<BYTE*>(&ipv6);
    } else {
        wideHost.assign(host.begin(), host.end());
        altName.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
        altName.pwszDNSName = const_cast<LPWSTR>(wideHost.c_str());
    }

    CERT_ALT_NAME_INFO altNames = {};
    altNames.cAltEntry = 1;
    altNames.rgAltEntry = &altName;

    if (!EncodeObject(X509_ALTERNATE_NAME, &altNames, encoded)) {
        return false;
    }

    extension.pszObjId = const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2);
    extension.fCritical = FALSE;
    extension.Value = encoded.AsBlob();
    return true;
}

PCCERT_CONTEXT CreateSignedCertificate(HCRYPTPROV signerProv,
                                       DWORD signerKeySpec,
                                       PCERT_INFO certInfo) {
    CRYPT_ALGORITHM_IDENTIFIER signatureAlgorithm = {};
    signatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    DWORD encodedSize = 0;
    if (!CryptSignAndEncodeCertificate(signerProv, signerKeySpec, kAsnEncoding,
                                       X509_CERT_TO_BE_SIGNED, certInfo,
                                       &signatureAlgorithm, nullptr,
                                       nullptr, &encodedSize)) {
        LogCryptoFailure("CryptSignAndEncodeCertificate(size)");
        return nullptr;
    }

    BYTE* encodedData = static_cast<BYTE*>(LocalAlloc(LMEM_FIXED, encodedSize));
    if (!encodedData) {
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[SSL-MITM] LocalAlloc failed while signing certificate");
        return nullptr;
    }

    PCCERT_CONTEXT cert = nullptr;
    if (CryptSignAndEncodeCertificate(signerProv, signerKeySpec, kAsnEncoding,
                                      X509_CERT_TO_BE_SIGNED, certInfo,
                                      &signatureAlgorithm, nullptr,
                                      encodedData, &encodedSize)) {
        cert = CertCreateCertificateContext(kAsnEncoding, encodedData, encodedSize);
        if (!cert) {
            LogCryptoFailure("CertCreateCertificateContext");
        }
    } else {
        LogCryptoFailure("CryptSignAndEncodeCertificate(sign)");
    }

    LocalFree(encodedData);
    return cert;
}

bool BindPrivateKeyToCertificate(PCCERT_CONTEXT cert, HCRYPTPROV hProv, DWORD keySpec) {
    CERT_KEY_CONTEXT keyContext = {};
    keyContext.cbSize = sizeof(keyContext);
    keyContext.hCryptProv = hProv;
    keyContext.dwKeySpec = keySpec;

    return CertSetCertificateContextProperty(
        cert, CERT_KEY_CONTEXT_PROP_ID, 0, &keyContext) == TRUE;
}
} // namespace

// ============================================================
// 全局 CA 静态成员
// ============================================================
PCCERT_CONTEXT SSLMitmContext::s_caCert  = nullptr;
HCRYPTPROV     SSLMitmContext::s_hProv   = 0;
HCRYPTKEY      SSLMitmContext::s_hCaKey  = 0;
std::mutex     SSLMitmContext::s_caMutex;
bool           SSLMitmContext::s_caInited = false;

// ============================================================
// 工具：向 SOCKET 可靠发送所有字节
// ============================================================
static bool SendAll(SOCKET s, const uint8_t* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(s, reinterpret_cast<const char*>(buf + sent), len - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

// ============================================================
// InitGlobalCA — 生成自签名 CA 证书（进程级，只调用一次）
// ============================================================
bool SSLMitmContext::InitGlobalCA() {
    std::lock_guard<std::mutex> lk(s_caMutex);
    if (s_caInited) return true;
    SetLastSslMitmErrorDetail("");

    // 打开或创建真实密钥容器。根据官方文档，私钥操作不应使用 CRYPT_VERIFYCONTEXT。
    if (!CryptAcquireContextW(&s_hProv, kCaKeyContainerName, MS_ENH_RSA_AES_PROV_W,
                              PROV_RSA_AES, 0)) {
        const DWORD openError = GetLastError();
        if (openError != NTE_BAD_KEYSET ||
            !CryptAcquireContextW(&s_hProv, kCaKeyContainerName, MS_ENH_RSA_AES_PROV_W,
                                  PROV_RSA_AES, CRYPT_NEWKEYSET)) {
            LogCryptoFailure("CryptAcquireContextW(CA container)", openError == NTE_BAD_KEYSET ? GetLastError() : openError);
            return false;
        }
    }

    HCRYPTKEY hKey = 0;
    if (!CryptGetUserKey(s_hProv, AT_SIGNATURE, &hKey)) {
        const DWORD getKeyError = GetLastError();
        if ((getKeyError != NTE_NO_KEY && getKeyError != NTE_BAD_KEYSET) ||
            !CryptGenKey(s_hProv, AT_SIGNATURE, kRsa2048Flags, &hKey)) {
            LogCryptoFailure("CryptGenKey(CA AT_SIGNATURE)", getKeyError == NTE_NO_KEY || getKeyError == NTE_BAD_KEYSET ? GetLastError() : getKeyError);
            CryptReleaseContext(s_hProv, 0);
            s_hProv = 0;
            return false;
        }
    }

    AutoLocalBuffer subjectName;
    if (!BuildSubjectNameBlob("SSL-MITM-CA", subjectName)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(s_hProv, 0);
        s_hProv = 0;
        return false;
    }

    CERT_NAME_BLOB subjectBlob = {
        subjectName.size,
        subjectName.data
    };

    // 有效期 10 年
    SYSTEMTIME st, et;
    GetSystemTime(&st);
    et = st;
    et.wYear += 10;

    CERT_EXTENSION extensions[2] = {};
    AutoLocalBuffer basicConstraints;
    AutoLocalBuffer keyUsage;
    if (!BuildBasicConstraintsExtension(true, extensions[0], basicConstraints) ||
        !BuildKeyUsageExtension(
            CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE,
            extensions[1], keyUsage)) {
        SetLastSslMitmErrorDetail("[SSL-MITM] Failed to build CA certificate extensions");
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[SSL-MITM] Failed to build CA certificate extensions");
        CryptDestroyKey(hKey);
        CryptReleaseContext(s_hProv, 0);
        s_hProv = 0;
        return false;
    }

    AutoLocalBuffer publicKeyStorage;
    PCERT_PUBLIC_KEY_INFO publicKeyInfo = nullptr;
    if (!ExportPublicKeyInfo(s_hProv, AT_SIGNATURE, publicKeyStorage, publicKeyInfo)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(s_hProv, 0);
        s_hProv = 0;
        return false;
    }

    std::array<BYTE, 16> serialBytes = {};
    CRYPT_INTEGER_BLOB serialNumber = {};
    if (!CreateSerialNumber(s_hProv, serialBytes, serialNumber)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(s_hProv, 0);
        s_hProv = 0;
        return false;
    }

    FILETIME notBefore = {};
    FILETIME notAfter = {};
    if (!SystemTimeToFileTime(&st, &notBefore) ||
        !SystemTimeToFileTime(&et, &notAfter)) {
        SetLastSslMitmErrorDetail("[SSL-MITM] SystemTimeToFileTime failed while building CA certificate");
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[SSL-MITM] SystemTimeToFileTime failed while building CA certificate");
        CryptDestroyKey(hKey);
        CryptReleaseContext(s_hProv, 0);
        s_hProv = 0;
        return false;
    }

    CERT_EXTENSIONS certExtensions = {};
    certExtensions.cExtension = 2;
    certExtensions.rgExtension = extensions;

    CRYPT_ALGORITHM_IDENTIFIER signatureAlgorithm = {};
    signatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    CERT_INFO certInfo = {};
    certInfo.dwVersion = CERT_V3;
    certInfo.SerialNumber = serialNumber;
    certInfo.SignatureAlgorithm = signatureAlgorithm;
    certInfo.Issuer = subjectBlob;
    certInfo.NotBefore = notBefore;
    certInfo.NotAfter = notAfter;
    certInfo.Subject = subjectBlob;
    certInfo.SubjectPublicKeyInfo = *publicKeyInfo;
    certInfo.cExtension = certExtensions.cExtension;
    certInfo.rgExtension = certExtensions.rgExtension;

    s_caCert = CreateSignedCertificate(s_hProv, AT_SIGNATURE, &certInfo);

    if (!s_caCert) {
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[SSL-MITM] Failed to create self-signed CA certificate via manual signing");
        CryptDestroyKey(hKey);
        CryptReleaseContext(s_hProv, 0);
        s_hProv = 0;
        return false;
    }

    s_hCaKey = hKey;
    s_caInited = true;
    SetLastSslMitmErrorDetail("");
    return true;
}

// ============================================================
// ExportCACert — 导出 CA 为 DER 文件供用户安装
// ============================================================
bool SSLMitmContext::ExportCACert(const std::string& filePath) {
    std::lock_guard<std::mutex> lk(s_caMutex);
    if (!s_caCert) {
        SetLastSslMitmErrorDetail("[SSL-MITM] CA certificate is not initialized");
        return false;
    }

    HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogCryptoFailure("CreateFileA(export CA certificate)");
        return false;
    }

    DWORD written = 0;
    if (!WriteFile(hFile, s_caCert->pbCertEncoded,
                   s_caCert->cbCertEncoded, &written, nullptr)) {
        CloseHandle(hFile);
        LogCryptoFailure("WriteFile(export CA certificate)");
        return false;
    }
    CloseHandle(hFile);
    if (written != s_caCert->cbCertEncoded) {
        std::ostringstream oss;
        oss << "[SSL-MITM] CA certificate write incomplete (written=" << written
            << ", expected=" << s_caCert->cbCertEncoded << ")";
        SetLastSslMitmErrorDetail(oss.str());
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, oss.str());
        return false;
    }
    SetLastSslMitmErrorDetail("");
    return true;
}

std::string SSLMitmContext::GetLastErrorDetail() {
    return GetLastSslMitmErrorDetailCopy();
}

// ============================================================
// IssueCert — 用 CA 为 cn 签发叶子证书
// ============================================================
PCCERT_CONTEXT SSLMitmContext::IssueCert(const std::string& cn) {
    std::lock_guard<std::mutex> lk(s_caMutex);
    if (!s_caCert || !s_hProv) return nullptr;

    AutoLocalBuffer subjectName;
    if (!BuildSubjectNameBlob(cn, subjectName)) {
        return nullptr;
    }
    CERT_NAME_BLOB subjectBlob = {
        subjectName.size,
        subjectName.data
    };

    // 有效期 2 年
    SYSTEMTIME st, et;
    GetSystemTime(&st);
    et = st;
    et.wYear += 2;

    FILETIME notBefore = {};
    FILETIME notAfter = {};
    if (!SystemTimeToFileTime(&st, &notBefore) ||
        !SystemTimeToFileTime(&et, &notAfter)) {
        return nullptr;
    }

    // 生成叶子密钥（供 Schannel 服务端握手使用）
    HCRYPTPROV hLeafProv = 0;
    if (!CryptAcquireContextW(&hLeafProv, nullptr, MS_DEF_RSA_SCHANNEL_PROV_W,
                              PROV_RSA_SCHANNEL, CRYPT_VERIFYCONTEXT)) {
        return nullptr;
    }

    HCRYPTKEY hLeafKey = 0;
    if (!CryptGenKey(hLeafProv, AT_KEYEXCHANGE, kRsa2048Flags, &hLeafKey)) {
        CryptReleaseContext(hLeafProv, 0);
        return nullptr;
    }

    AutoLocalBuffer publicKeyStorage;
    PCERT_PUBLIC_KEY_INFO publicKeyInfo = nullptr;
    if (!ExportPublicKeyInfo(hLeafProv, AT_KEYEXCHANGE, publicKeyStorage, publicKeyInfo)) {
        CryptDestroyKey(hLeafKey);
        CryptReleaseContext(hLeafProv, 0);
        return nullptr;
    }

    CERT_EXTENSION extensions[4] = {};
    AutoLocalBuffer basicConstraints;
    AutoLocalBuffer keyUsage;
    AutoLocalBuffer enhancedKeyUsage;
    AutoLocalBuffer subjectAltName;
    if (!BuildBasicConstraintsExtension(false, extensions[0], basicConstraints) ||
        !BuildKeyUsageExtension(
            CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE,
            extensions[1], keyUsage) ||
        !BuildEnhancedKeyUsageExtension(extensions[2], enhancedKeyUsage) ||
        !BuildSubjectAltNameExtension(cn, extensions[3], subjectAltName)) {
        CryptDestroyKey(hLeafKey);
        CryptReleaseContext(hLeafProv, 0);
        return nullptr;
    }

    std::array<BYTE, 16> serialBytes = {};
    CRYPT_INTEGER_BLOB serialNumber = {};
    if (!CreateSerialNumber(s_hProv, serialBytes, serialNumber)) {
        CryptDestroyKey(hLeafKey);
        CryptReleaseContext(hLeafProv, 0);
        return nullptr;
    }

    CRYPT_ALGORITHM_IDENTIFIER signatureAlgorithm = {};
    signatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    CERT_INFO certInfo = {};
    certInfo.dwVersion = CERT_V3;
    certInfo.SerialNumber = serialNumber;
    certInfo.SignatureAlgorithm = signatureAlgorithm;
    certInfo.Issuer = s_caCert->pCertInfo->Subject;
    certInfo.NotBefore = notBefore;
    certInfo.NotAfter = notAfter;
    certInfo.Subject = subjectBlob;
    certInfo.SubjectPublicKeyInfo = *publicKeyInfo;
    certInfo.cExtension = 4;
    certInfo.rgExtension = extensions;

    PCCERT_CONTEXT leafCert = CreateSignedCertificate(s_hProv, AT_SIGNATURE, &certInfo);
    if (!leafCert) {
        CryptDestroyKey(hLeafKey);
        CryptReleaseContext(hLeafProv, 0);
        return nullptr;
    }

    if (!BindPrivateKeyToCertificate(leafCert, hLeafProv, AT_KEYEXCHANGE)) {
        CertFreeCertificateContext(leafCert);
        CryptDestroyKey(hLeafKey);
        CryptReleaseContext(hLeafProv, 0);
        return nullptr;
    }

    CryptDestroyKey(hLeafKey);
    hLeafProv = 0; // 交由证书上下文生命周期托管
    return leafCert;
}

// ============================================================
// FreeSide
// ============================================================
void SSLMitmContext::FreeSide(SchannelSide& side) {
    if (side.ctxOk) {
        DeleteSecurityContext(&side.hCtx);
        side.ctxOk = false;
    }
    if (side.credOk) {
        FreeCredentialsHandle(&side.hCred);
        side.credOk = false;
    }
    if (side.ownedCert) {
        CertFreeCertificateContext(side.ownedCert);
        side.ownedCert = nullptr;
    }
}

void SSLMitmContext::Shutdown() {
    FreeSide(client_);
    FreeSide(server_);
}

// ============================================================
// HandshakeAsClient — 对真实服务器做 TLS 客户端握手
// ============================================================
bool SSLMitmContext::HandshakeAsClient(SOCKET sock,
                                        const std::string& host,
                                        SchannelSide& side) {
    // 凭据：客户端，不验证服务器证书（内网/MITM 场景）
    SCHANNEL_CRED cred = {};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags   = SCH_CRED_NO_DEFAULT_CREDS
                   | SCH_CRED_MANUAL_CRED_VALIDATION; // 跳过证书校验

    SECURITY_STATUS ss = AcquireCredentialsHandleA(
        nullptr, const_cast<LPSTR>(UNISP_NAME_A),
        SECPKG_CRED_OUTBOUND, nullptr,
        &cred, nullptr, nullptr,
        &side.hCred, nullptr);
    if (ss != SEC_E_OK) return false;
    side.credOk = true;

    // 握手循环
    bool firstCall = true;
    SecBufferDesc inDesc, outDesc;
    SecBuffer inBuf[2], outBuf[1];
    DWORD ctxAttr = 0;

    std::vector<uint8_t> recvData;
    recvData.reserve(16384);

    while (true) {
        // 输出 token
        outBuf[0] = { 0, SECBUFFER_TOKEN, nullptr };
        outDesc    = { SECBUFFER_VERSION, 1, outBuf };

        SecBufferDesc* pIn = nullptr;
        if (!firstCall) {
            inBuf[0] = { static_cast<ULONG>(recvData.size()),
                         SECBUFFER_TOKEN, recvData.data() };
            inBuf[1] = { 0, SECBUFFER_EMPTY, nullptr };
            inDesc   = { SECBUFFER_VERSION, 2, inBuf };
            pIn      = &inDesc;
        }

        ss = InitializeSecurityContextA(
            &side.hCred,
            firstCall ? nullptr : &side.hCtx,
            const_cast<LPSTR>(host.c_str()),
            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
            ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM,
            0, SECURITY_NATIVE_DREP,
            pIn, 0,
            &side.hCtx, &outDesc,
            &ctxAttr, nullptr);
        firstCall = false;
        side.ctxOk = true;

        // 发送生成的 token
        if (outBuf[0].cbBuffer > 0 && outBuf[0].pvBuffer) {
            SendAll(sock,
                reinterpret_cast<uint8_t*>(outBuf[0].pvBuffer),
                outBuf[0].cbBuffer);
            FreeContextBuffer(outBuf[0].pvBuffer);
        }

        if (ss == SEC_E_OK) {
            // 握手完成，处理 inBuf[1] 中可能剩余的数据
            if (inBuf[1].BufferType == SECBUFFER_EXTRA && inBuf[1].cbBuffer > 0) {
                size_t offset = recvData.size() - inBuf[1].cbBuffer;
                side.recvBuf.insert(side.recvBuf.end(),
                    recvData.begin() + offset, recvData.end());
            }
            break;
        }
        if (ss != SEC_I_CONTINUE_NEEDED && ss != SEC_E_INCOMPLETE_MESSAGE) {
            return false;
        }

        // 从服务器接收更多数据
        char tmp[4096];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        recvData.insert(recvData.end(), tmp, tmp + n);
    }

    QueryContextAttributes(&side.hCtx,
        SECPKG_ATTR_STREAM_SIZES, &side.streamSizes);
    side.streamSizesOk = true;
    return true;
}

// ============================================================
// HandshakeAsServer — 对客户端做 TLS 服务端握手
// ============================================================
bool SSLMitmContext::HandshakeAsServer(SOCKET sock,
                                        const std::string& cn,
                                        SchannelSide& side) {
    // 为该域名签发证书
    PCCERT_CONTEXT leafCert = IssueCert(cn);
    if (!leafCert) return false;

    SCHANNEL_CRED cred = {};
    cred.dwVersion      = SCHANNEL_CRED_VERSION;
    cred.cCreds         = 1;
    cred.paCred         = &leafCert;
    cred.dwFlags        = SCH_CRED_NO_SYSTEM_MAPPER;

    SECURITY_STATUS ss = AcquireCredentialsHandleA(
        nullptr, const_cast<LPSTR>(UNISP_NAME_A),
        SECPKG_CRED_INBOUND, nullptr,
        &cred, nullptr, nullptr,
        &side.hCred, nullptr);

    if (ss != SEC_E_OK) {
        CertFreeCertificateContext(leafCert);
        return false;
    }
    side.credOk = true;
    side.ownedCert = leafCert;

    // 握手循环
    SecBufferDesc inDesc, outDesc;
    SecBuffer inBuf[2], outBuf[1];
    DWORD ctxAttr = 0;
    bool firstCall = true;

    std::vector<uint8_t> recvData;
    recvData.reserve(16384);

    while (true) {
        // 先从客户端接收数据
        char tmp[4096];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        recvData.insert(recvData.end(), tmp, tmp + n);

        inBuf[0] = { static_cast<ULONG>(recvData.size()),
                     SECBUFFER_TOKEN, recvData.data() };
        inBuf[1] = { 0, SECBUFFER_EMPTY, nullptr };
        inDesc   = { SECBUFFER_VERSION, 2, inBuf };

        outBuf[0] = { 0, SECBUFFER_TOKEN, nullptr };
        outDesc   = { SECBUFFER_VERSION, 1, outBuf };

        ss = AcceptSecurityContext(
            &side.hCred,
            firstCall ? nullptr : &side.hCtx,
            &inDesc,
            ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
            ASC_REQ_CONFIDENTIALITY | ASC_REQ_STREAM,
            SECURITY_NATIVE_DREP,
            &side.hCtx, &outDesc,
            &ctxAttr, nullptr);
        firstCall = false;
        side.ctxOk = true;

        // 发送 token
        if (outBuf[0].cbBuffer > 0 && outBuf[0].pvBuffer) {
            SendAll(sock,
                reinterpret_cast<uint8_t*>(outBuf[0].pvBuffer),
                outBuf[0].cbBuffer);
            FreeContextBuffer(outBuf[0].pvBuffer);
        }

        if (ss == SEC_E_OK) {
            if (inBuf[1].BufferType == SECBUFFER_EXTRA && inBuf[1].cbBuffer > 0) {
                size_t offset = recvData.size() - inBuf[1].cbBuffer;
                side.recvBuf.insert(side.recvBuf.end(),
                    recvData.begin() + offset, recvData.end());
            }
            break;
        }
        if (ss == SEC_I_CONTINUE_NEEDED) {
            // 处理 EXTRA
            if (inBuf[1].BufferType == SECBUFFER_EXTRA && inBuf[1].cbBuffer > 0) {
                size_t offset = recvData.size() - inBuf[1].cbBuffer;
                std::vector<uint8_t> extra(recvData.begin() + offset, recvData.end());
                recvData = std::move(extra);
            } else {
                recvData.clear();
            }
            continue;
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            // 继续读取更多数据
            continue;
        }
        return false;
    }

    QueryContextAttributes(&side.hCtx,
        SECPKG_ATTR_STREAM_SIZES, &side.streamSizes);
    side.streamSizesOk = true;
    return true;
}

// ============================================================
// Handshake — 完整 MITM 握手：先连服务器，再欺骗客户端
// ============================================================
bool SSLMitmContext::Handshake(SOCKET clientSock, SOCKET serverSock,
                                const std::string& targetHost) {
    clientSock_  = clientSock;
    serverSock_  = serverSock;
    targetHost_  = targetHost;

    // 步骤1：对真实服务器做 TLS 客户端握手
    if (!HandshakeAsClient(serverSock_, targetHost_, server_)) {
        return false;
    }

    // 步骤2：对客户端做 TLS 服务端握手（用动态证书）
    if (!HandshakeAsServer(clientSock_, targetHost_, client_)) {
        return false;
    }

    return true;
}

// ============================================================
// SchannelRecvDecrypt — 从 socket 读取加密数据并解密
// ============================================================
int SSLMitmContext::SchannelRecvDecrypt(SOCKET sock, SchannelSide& side) {
    char tmp[8192];
    int n = recv(sock, tmp, sizeof(tmp), 0);
    if (n <= 0) return (n == 0) ? -1 : -1;

    side.recvBuf.insert(side.recvBuf.end(), tmp, tmp + n);

    int totalDecrypted = 0;

    while (!side.recvBuf.empty()) {
        SecBuffer databufs[4];
        databufs[0] = { static_cast<ULONG>(side.recvBuf.size()),
                        SECBUFFER_DATA, side.recvBuf.data() };
        databufs[1] = { 0, SECBUFFER_EMPTY, nullptr };
        databufs[2] = { 0, SECBUFFER_EMPTY, nullptr };
        databufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, databufs };

        SECURITY_STATUS ss = DecryptMessage(&side.hCtx, &desc, 0, nullptr);

        if (ss == SEC_E_OK || ss == SEC_I_RENEGOTIATE) {
            for (int i = 0; i < 4; i++) {
                if (databufs[i].BufferType == SECBUFFER_DATA && databufs[i].cbBuffer > 0) {
                    auto* p = reinterpret_cast<uint8_t*>(databufs[i].pvBuffer);
                    side.plainBuf.insert(side.plainBuf.end(), p, p + databufs[i].cbBuffer);
                    totalDecrypted += databufs[i].cbBuffer;
                }
            }
            // 处理剩余加密数据
            side.recvBuf.clear();
            for (int i = 0; i < 4; i++) {
                if (databufs[i].BufferType == SECBUFFER_EXTRA && databufs[i].cbBuffer > 0) {
                    auto* p = reinterpret_cast<uint8_t*>(databufs[i].pvBuffer);
                    side.recvBuf.insert(side.recvBuf.end(), p, p + databufs[i].cbBuffer);
                }
            }
            if (ss == SEC_I_RENEGOTIATE) break; // 重协商，暂不处理
        } else if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            break; // 需要更多数据
        } else {
            return -1; // 错误
        }
    }

    return totalDecrypted > 0 ? totalDecrypted : 0;
}

// ============================================================
// SchannelEncryptSend — 加密并发送明文数据
// ============================================================
bool SSLMitmContext::SchannelEncryptSend(SOCKET sock, SchannelSide& side,
                                          const uint8_t* plain, int len) {
    if (!side.streamSizesOk) return false;

    const ULONG maxMsg = side.streamSizes.cbMaximumMessage;
    int offset = 0;

    while (offset < len) {
        int chunk = (int)min((ULONG)(len - offset), maxMsg);

        std::vector<uint8_t> buf(
            side.streamSizes.cbHeader + chunk + side.streamSizes.cbTrailer);

        memcpy(buf.data() + side.streamSizes.cbHeader, plain + offset, chunk);

        SecBuffer encBufs[4];
        encBufs[0] = { side.streamSizes.cbHeader,  SECBUFFER_STREAM_HEADER,
                       buf.data() };
        encBufs[1] = { static_cast<ULONG>(chunk),   SECBUFFER_DATA,
                       buf.data() + side.streamSizes.cbHeader };
        encBufs[2] = { side.streamSizes.cbTrailer,  SECBUFFER_STREAM_TRAILER,
                       buf.data() + side.streamSizes.cbHeader + chunk };
        encBufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, encBufs };

        SECURITY_STATUS ss = EncryptMessage(&side.hCtx, 0, &desc, 0);
        if (ss != SEC_E_OK) return false;

        ULONG totalSize = encBufs[0].cbBuffer + encBufs[1].cbBuffer + encBufs[2].cbBuffer;
        if (!SendAll(sock, buf.data(), (int)totalSize)) return false;

        offset += chunk;
    }
    return true;
}

// ============================================================
// 公共读写接口
// ============================================================
int SSLMitmContext::ReadFromClient(std::vector<uint8_t>& outPlain) {
    if (!client_.plainBuf.empty()) {
        outPlain = std::move(client_.plainBuf);
        client_.plainBuf.clear();
        return (int)outPlain.size();
    }
    int r = SchannelRecvDecrypt(clientSock_, client_);
    if (r < 0) return -1;
    if (!client_.plainBuf.empty()) {
        outPlain = std::move(client_.plainBuf);
        client_.plainBuf.clear();
        return (int)outPlain.size();
    }
    return 0;
}

bool SSLMitmContext::WriteToClient(const uint8_t* data, int len) {
    return SchannelEncryptSend(clientSock_, client_, data, len);
}

int SSLMitmContext::ReadFromServer(std::vector<uint8_t>& outPlain) {
    if (!server_.plainBuf.empty()) {
        outPlain = std::move(server_.plainBuf);
        server_.plainBuf.clear();
        return (int)outPlain.size();
    }
    int r = SchannelRecvDecrypt(serverSock_, server_);
    if (r < 0) return -1;
    if (!server_.plainBuf.empty()) {
        outPlain = std::move(server_.plainBuf);
        server_.plainBuf.clear();
        return (int)outPlain.size();
    }
    return 0;
}

bool SSLMitmContext::WriteToServer(const uint8_t* data, int len) {
    return SchannelEncryptSend(serverSock_, server_, data, len);
}
