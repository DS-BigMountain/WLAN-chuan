#include "core.h"
#include "transfer_view.h"
#include <algorithm>
#include <bcrypt.h>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iphlpapi.h>
#include <limits>
#include <netioapi.h>
#include <shlobj.h>
#include <sstream>
#include <stdexcept>
#include <ws2tcpip.h>

namespace chuan {
namespace {
uint64_t Now() {
    return GetTickCount64();
}
struct WinFile {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit WinFile(HANDLE h) : value(h) {}
    ~WinFile() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct Hash {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    Hash() {
        DWORD bytes = 0, count = 0;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("SHA256");
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes),
                              &count, 0) < 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            alg = nullptr;
            throw std::runtime_error("SHA256");
        }
        object.resize(bytes);
        if (BCryptCreateHash(alg, &hash, object.data(), bytes, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            alg = nullptr;
            throw std::runtime_error("SHA256");
        }
    }
    ~Hash() {
        if (hash)
            BCryptDestroyHash(hash);
        if (alg)
            BCryptCloseAlgorithmProvider(alg, 0);
    }
    void Add(const void *data, size_t size) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void *>(data)), static_cast<ULONG>(size),
                           0) < 0)
            throw std::runtime_error("SHA256");
    }
    std::array<unsigned char, 32> Finish() {
        std::array<unsigned char, 32> out{};
        if (BCryptFinishHash(hash, out.data(), 32, 0) < 0)
            throw std::runtime_error("SHA256");
        return out;
    }
};
std::wstring Guid() {
    GUID id{};
    CoCreateGuid(&id);
    wchar_t value[40]{};
    StringFromGUID2(id, value, 40);
    return value;
}
void SetTimeout(SOCKET s, DWORD receive = 30000, DWORD sendTime = 30000) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&receive), sizeof(receive));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&sendTime), sizeof(sendTime));
}
bool SendAll(SOCKET s, const void *ptr, size_t length) {
    auto p = static_cast<const char *>(ptr);
    while (length) {
        int n = send(s, p, static_cast<int>(std::min<size_t>(length, 256 * 1024)), 0);
        if (n <= 0)
            return false;
        p += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}
bool SendAll(SOCKET s, const std::string &text) {
    return SendAll(s, text.data(), text.size());
}
bool ReadExact(SOCKET s, void *ptr, size_t length) {
    auto p = static_cast<char *>(ptr);
    while (length) {
        int n = recv(s, p, static_cast<int>(std::min<size_t>(length, 256 * 1024)), 0);
        if (n <= 0)
            return false;
        p += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}
bool ReadHeader(SOCKET s, std::string &first, std::map<std::string, std::string> &headers) {
    std::string all;
    char c = 0;
    while (all.size() < 16384) {
        if (recv(s, &c, 1, 0) != 1)
            return false;
        all += c;
        if (all.size() >= 4 && all.compare(all.size() - 4, 4, "\r\n\r\n") == 0)
            break;
    }
    if (all.size() >= 16384)
        return false;
    std::istringstream lines(all);
    std::getline(lines, first);
    if (!first.empty() && first.back() == '\r')
        first.pop_back();
    std::string line;
    while (std::getline(lines, line)) {
        if (line == "\r" || line.empty())
            break;
        auto p = line.find(':');
        if (p == std::string::npos)
            return false;
        std::string key = line.substr(0, p), value = line.substr(p + 1);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char v) { return static_cast<char>(std::tolower(v)); });
        while (!value.empty() && value.front() == ' ')
            value.erase(value.begin());
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
        if (headers.contains(key))
            return false;
        headers[key] = value;
    }
    return true;
}
void Reply(SOCKET s, int status, const std::string &reason, const std::string &body = "",
           const std::string &type = "text/plain; charset=utf-8") {
    SendAll(s, "HTTP/1.1 " + std::to_string(status) + " " + reason +
                   "\r\nConnection: close\r\nContent-Type: " + type +
                   "\r\nContent-Length: " + std::to_string(body.size()) +
                   "\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\n\r\n" + body);
}
std::string Get(const std::map<std::string, std::string> &h, const std::string &key) {
    auto i = h.find(key);
    return i == h.end() ? "" : i->second;
}
bool Number(const std::string &s, uint64_t &result) {
    if (s.empty() || s.size() > 20)
        return false;
    result = 0;
    for (unsigned char c : s) {
        if (c < '0' || c > '9')
            return false;
        unsigned int n = c - '0';
        if (result > (std::numeric_limits<uint64_t>::max() - n) / 10)
            return false;
        result = result * 10 + n;
    }
    return true;
}
std::string Json(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c < 32)
            out += ' ';
        else
            out += c;
    }
    return out + '"';
}
bool Endpoint(const std::string &text, std::string &ip, unsigned short &port) {
    auto pos = text.find(':');
    ip = text.substr(0, pos);
    std::wstring value = pos == std::string::npos ? std::to_wstring(DefaultPort) : Wide(text.substr(pos + 1));
    in_addr addr{};
    return InetPtonA(AF_INET, ip.c_str(), &addr) == 1 && ValidPort(value, port);
}
SOCKET Connect(const std::string &ip, unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return s;
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (InetPtonA(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    int ret = connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (ret != 0) {
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(s, &wf);
        FD_SET(s, &ef);
        timeval tv{3, 0};
        if (select(0, nullptr, &wf, &ef, &tv) <= 0 || FD_ISSET(s, &ef)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        int err = 0, len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len);
        if (err) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    mode = 0;
    ioctlsocket(s, FIONBIO, &mode);
    SetTimeout(s);
    return s;
}
bool SafeParents(const fs::path &root, const fs::path &parent) {
    fs::path current = root;
    if (GetFileAttributesW(current.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
        return false;
    auto rel = parent.lexically_relative(root);
    for (const auto &part : rel) {
        if (part == L".")
            continue;
        if (part == L"..")
            return false;
        current /= part;
        std::error_code ec;
        fs::create_directory(current, ec);
        DWORD attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            return false;
    }
    return true;
}
fs::path UniquePath(const fs::path &path, unsigned int suffix) {
    if (!suffix)
        return path;
    return path.parent_path() /
           (path.stem().wstring() + L" (" + std::to_wstring(suffix) + L")" + path.extension().wstring());
}
std::string WebResource(int id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource)
        return {};
    auto data = static_cast<const char *>(LockResource(LoadResource(module, resource)));
    return data ? std::string(data, SizeofResource(module, resource)) : std::string{};
}
void WebReply(SOCKET s, const std::string &body, const std::string &type) {
    SendAll(s, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: " + type +
                   "\r\nContent-Length: " + std::to_string(body.size()) +
                   "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                   "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; "
                   "object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n\r\n" +
                   body);
}
bool WebKey(const std::string &s) {
    return s.size() == 32 && Hex(Unhex(s)) == s;
}
bool SafeWebPath(const fs::path &path, const fs::path &root) {
    auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || relative.is_absolute())
        return false;
    for (auto &part : relative)
        if (part == L"..")
            return false;
    fs::path current = path.root_path();
    for (auto &part : path.relative_path()) {
        current /= part;
        DWORD attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    return true;
}
bool WebHandleMatches(HANDLE handle, const fs::path &path) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        return false;
    std::wstring expected = path.lexically_normal().wstring();
    if (!expected.starts_with(L"\\\\?\\"))
        expected =
            expected.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + expected.substr(2) : L"\\\\?\\" + expected;
    DWORD length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!length)
        return false;
    std::wstring actual(length, 0);
    DWORD used = GetFinalPathNameByHandleW(handle, actual.data(), length, FILE_NAME_NORMALIZED);
    if (!used || used >= length)
        return false;
    actual.resize(used);
    return _wcsicmp(actual.c_str(), expected.c_str()) == 0;
}
std::string PercentEncode(const std::string &s) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}
} // namespace

std::string Utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr,
                                0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}
std::wstring Wide(const std::string &s) {
    if (s.empty())
        return {};
    int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string Hex(const std::string &s) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}
std::string Unhex(const std::string &s) {
    if (s.size() % 2)
        return {};
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = digit(s[i]), b = digit(s[i + 1]);
        if (a < 0 || b < 0)
            return {};
        out += static_cast<char>((a << 4) | b);
    }
    return out;
}
std::wstring FormatBytes(uint64_t n) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(1);
    if (n >= 1024ull * 1024 * 1024)
        out << static_cast<double>(n) / (1024 * 1024 * 1024) << L" GB";
    else if (n >= 1024 * 1024)
        out << static_cast<double>(n) / (1024 * 1024) << L" MB";
    else if (n >= 1024)
        out << static_cast<double>(n) / 1024 << L" KB";
    else
        out << n << L" B";
    return out.str();
}
bool ValidPort(const std::wstring &s, unsigned short &result) {
    uint64_t n = 0;
    if (!Number(Utf8(s), n) || n < 1024 || n > 65535)
        return false;
    result = static_cast<unsigned short>(n);
    return true;
}
std::wstring SanitizeRelative(const std::wstring &input) {
    if (input.empty() || input.size() > 1800 || input.front() == L'/' || input.front() == L'\\')
        return {};
    std::wstring normalized = input;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    std::wistringstream parts(normalized);
    std::wstring part, out;
    while (std::getline(parts, part, L'\\')) {
        if (part.empty() || part == L"." || part == L".." || part.size() > 180 || part.back() == L'.' ||
            part.back() == L' ')
            return {};
        for (wchar_t c : part)
            if (c < 32 || std::wstring(L"<>:\"|?*").find(c) != std::wstring::npos)
                return {};
        std::wstring stem = part.substr(0, part.find(L'.'));
        while (!stem.empty() && stem.back() == L' ')
            stem.pop_back();
        std::transform(stem.begin(), stem.end(), stem.begin(), towupper);
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" ||
            stem == L"CONOUT$" ||
            (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
             (stem[3] >= L'0' && stem[3] <= L'9')) ||
            (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
             (stem[3] == L'¹' || stem[3] == L'²' || stem[3] == L'³')))
            return {};
        if (!out.empty())
            out += L'\\';
        out += part;
    }
    if (normalized.back() == L'\\')
        return {};
    return out;
}
fs::path ExePath() {
    std::wstring path(32768, 0);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return path;
}
Config LoadConfig(const fs::path &overrideDir) {
    Config c;
    PWSTR path = nullptr;
    if (overrideDir.empty()) {
        c.fallbackDataDir = ExePath().parent_path() / L"data";
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
            c.dataDir = fs::path(path) / L"Chuan";
            CoTaskMemFree(path);
        }
        std::error_code ec;
        if (fs::is_regular_file(c.fallbackDataDir / L"settings.ini", ec))
            c.dataDir = c.fallbackDataDir;
    } else
        c.dataDir = fs::absolute(overrideDir);
    if (c.dataDir.empty())
        c.dataDir = ExePath().parent_path() / L"data";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) {
        c.receiveDir = fs::path(path) / L"传";
        CoTaskMemFree(path);
    } else
        c.receiveDir = c.dataDir / L"received";
    wchar_t name[256]{};
    DWORD length = 256;
    GetComputerNameW(name, &length);
    c.name = name;
    c.id = Guid();
    auto ini = c.dataDir / L"settings.ini";
    wchar_t value[4096]{};
    auto read = [&](const wchar_t *key, const std::wstring &def) {
        GetPrivateProfileStringW(L"Chuan", key, def.c_str(), value, 4096, ini.c_str());
        return std::wstring(value);
    };
    c.name = read(L"Name", c.name);
    c.id = read(L"Id", c.id);
    c.receiveDir = read(L"ReceiveDir", c.receiveDir.wstring());
    unsigned short p;
    if (ValidPort(read(L"Port", std::to_wstring(c.port)), p))
        c.port = p;
    c.autoReceive = read(L"AutoReceive", L"1") == L"1";
    c.shareSoftware = read(L"ShareSoftware", L"1") == L"1";
    Number(Utf8(read(L"MaxTransferFiles", L"0")), c.maxTransferFiles);
    Number(Utf8(read(L"MaxTransferBytes", L"0")), c.maxTransferBytes);
    return c;
}
void SaveConfig(const Config &c) {
    fs::create_directories(c.dataDir);
    const auto ini = c.dataDir / L"settings.ini";
    if (!fs::exists(ini)) {
        WinFile file(CreateFileW(ini.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
            throw fs::filesystem_error("create settings", ini,
                                       std::error_code(GetLastError(), std::system_category()));
        const unsigned char bom[] = {0xff, 0xfe};
        DWORD written = 0;
        if (!WriteFile(file.value, bom, sizeof(bom), &written, nullptr))
            throw fs::filesystem_error("initialize settings", ini,
                                       std::error_code(GetLastError(), std::system_category()));
        if (written != sizeof(bom))
            throw fs::filesystem_error("initialize settings", ini,
                                       std::error_code(ERROR_WRITE_FAULT, std::system_category()));
    }
    auto write = [&](const wchar_t *key, const std::wstring &v) {
        if (!WritePrivateProfileStringW(L"Chuan", key, v.c_str(), ini.c_str()))
            throw fs::filesystem_error("write settings", ini,
                                       std::error_code(GetLastError(), std::system_category()));
    };
    write(L"Name", c.name);
    write(L"Id", c.id);
    write(L"ReceiveDir", c.receiveDir.wstring());
    write(L"Port", std::to_wstring(c.port));
    write(L"AutoReceive", c.autoReceive ? L"1" : L"0");
    write(L"ShareSoftware", c.shareSoftware ? L"1" : L"0");
    write(L"MaxTransferFiles", std::to_wstring(c.maxTransferFiles));
    write(L"MaxTransferBytes", std::to_wstring(c.maxTransferBytes));
}
namespace {
void CheckReceiveDirectory(const fs::path &path) {
    if (path.empty() || !path.is_absolute())
        throw fs::filesystem_error("absolute receive path required", path,
                                   std::error_code(ERROR_BAD_PATHNAME, std::system_category()));
    fs::create_directories(path);
    const auto probe = path / (L".chuan-write-" + Guid());
    WinFile file(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (file.value == INVALID_HANDLE_VALUE)
        throw fs::filesystem_error("write receive directory", path,
                                   std::error_code(GetLastError(), std::system_category()));
}
std::wstring StorageError(const std::wstring &operation, const fs::path &path,
                          const std::exception &exception) {
    std::wstring result = operation + L"\n路径：" + path.wstring();
    if (const auto *error = dynamic_cast<const fs::filesystem_error *>(&exception)) {
        wchar_t message[1024]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       error->code().value(), 0, message, 1024, nullptr);
        result += L"\n系统错误 " + std::to_wstring(error->code().value()) + L"：" + message;
    } else {
        result += L"\n原因：" + Wide(exception.what());
    }
    return result;
}
} // namespace
std::wstring StateText(State s, bool r) {
    switch (s) {
    case State::Queued:
        return L"排队中";
    case State::Waiting:
        return r ? L"等待确认" : L"等待对方确认";
    case State::Running:
        return r ? L"接收中" : L"发送中";
    case State::Completed:
        return L"已完成";
    case State::Cancelled:
        return L"已取消";
    case State::Rejected:
        return L"已拒绝";
    default:
        return L"失败";
    }
}

Engine::Engine(Config c) : config_(std::move(c)), webToken_(Hex(Utf8(Guid()))) {}
Engine::~Engine() {
    Stop();
}
void Engine::Notice(const std::wstring &text) {
    std::lock_guard lock(mutex_);
    notice_ = text;
}
bool Engine::Start(std::wstring &error) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) {
        error = L"网络初始化失败";
        return false;
    }
    winsock_ = true;
    ULONG bytes = 16384;
    std::vector<unsigned char> storage(bytes);
    ULONG result =
        GetAdaptersAddresses(AF_INET,
                             GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()), &bytes);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(bytes);
        result =
            GetAdaptersAddresses(AF_INET,
                                 GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                     GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()), &bytes);
    }
    if (result == NO_ERROR)
        for (auto a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET || u->OnLinkPrefixLength == 0 ||
                    u->OnLinkPrefixLength > 32)
                    continue;
                auto ip = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr;
                uint32_t host = ntohl(ip.s_addr), mask = u->OnLinkPrefixLength == 32
                                                             ? 0xffffffffu
                                                             : 0xffffffffu << (32 - u->OnLinkPrefixLength);
                char text[64]{};
                InetNtopA(AF_INET, &ip, text, 64);
                MIB_IF_ROW2 row{};
                row.InterfaceLuid = a->Luid;
                bool physical =
                    GetIfEntry2(&row) == NO_ERROR && row.InterfaceAndOperStatusFlags.HardwareInterface;
                std::wstring name = a->FriendlyName ? a->FriendlyName : L"网卡";
                bool virtualAdapter = !physical;
                unsigned priority = physical ? (a->FirstGatewayAddress ? 0 : 1) : 2;
                if ((host >> 16) == 0xa9fe || (host >> 16) == 0xc612)
                    priority = 3;
                networks_.push_back(
                    {host, mask, (host & mask) | ~mask, text, name, virtualAdapter, priority});
            }
        }
    std::stable_sort(networks_.begin(), networks_.end(), [](const auto &a, const auto &b) {
        if (a.virtualAdapter != b.virtualAdapter)
            return !a.virtualAdapter;
        return a.priority < b.priority;
    });
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) {
        error = L"无法创建网络服务";
        Stop();
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char *>(&exclusive),
               sizeof(exclusive));
    sockaddr_in listenAddr{};
    listenAddr.sin_family = AF_INET;
    listenAddr.sin_port = htons(config_.port);
    listenAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listener_, reinterpret_cast<sockaddr *>(&listenAddr), sizeof(listenAddr)) == SOCKET_ERROR ||
        listen(listener_, 16) == SOCKET_ERROR) {
        error = L"TCP 端口 " + std::to_wstring(config_.port) +
                L" 无法使用。请关闭同端口程序，或使用 --port 指定其他端口。";
        Stop();
        return false;
    }
    boundPort_ = config_.port;
    discovery_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discovery_ != INVALID_SOCKET) {
        BOOL reuse = TRUE, broadcast = TRUE;
        setsockopt(discovery_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&reuse), sizeof(reuse));
        setsockopt(discovery_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char *>(&broadcast),
                   sizeof(broadcast));
        BOOL reset = FALSE;
        DWORD count = 0;
        WSAIoctl(discovery_, _WSAIOW(IOC_VENDOR, 12), &reset, sizeof(reset), nullptr, 0, &count, nullptr,
                 nullptr);
        sockaddr_in d{};
        d.sin_family = AF_INET;
        d.sin_port = htons(config_.discoveryPort);
        d.sin_addr.s_addr = INADDR_ANY;
        if (bind(discovery_, reinterpret_cast<sockaddr *>(&d), sizeof(d)) == SOCKET_ERROR) {
            closesocket(discovery_);
            discovery_ = INVALID_SOCKET;
        }
    }
    std::wstring warning;
    try {
        CheckReceiveDirectory(config_.receiveDir);
    } catch (const std::exception &exception) {
        const auto originalError = StorageError(L"原接收目录不可用。", config_.receiveDir, exception);
        std::vector<fs::path> candidates{config_.dataDir};
        if (!config_.fallbackDataDir.empty() && config_.fallbackDataDir != config_.dataDir)
            candidates.push_back(config_.fallbackDataDir);
        std::wstring failures = originalError;
        bool recovered = false;
        for (const auto &directory : candidates) {
            const auto fallback = directory / L"received";
            try {
                CheckReceiveDirectory(fallback);
            } catch (const std::exception &fallbackError) {
                failures += L"\n\n" + StorageError(L"备用接收目录不可用。", fallback, fallbackError);
                continue;
            }
            config_.receiveDir = fallback;
            warning = originalError + L"\n已改用接收目录：" + fallback.wstring() +
                      L"\n可在“设置”中重新选择接收目录。";
            if (directory != config_.dataDir) {
                config_.dataDir = directory;
                warning += L"\n已启用便携配置：" + (directory / L"settings.ini").wstring();
            }
            recovered = true;
            break;
        }
        if (!recovered) {
            error = failures +
                    L"\n请恢复目录访问权限，或使用 --receive-dir 指定可写目录。";
            Stop();
            return false;
        }
    }
    try {
        SaveConfig(config_);
    } catch (const std::exception &exception) {
        if (!warning.empty()) warning += L"\n\n";
        warning += StorageError(L"设置未能保存，本次会话仍可运行。", config_.dataDir / L"settings.ini", exception) +
                   L"\n请检查该文件是否只读、被占用或缺少写入权限；本次设置可能无法在重启后保留。";
    }
    error.clear();
    if (!warning.empty()) Notice(warning);
    if (config_.name.empty()) AutoNameLocked(Utf8(config_.id));
    running_ = true;
    serverThread_ = std::thread([this] { ServerLoop(); });
    if (discovery_ != INVALID_SOCKET)
        discoveryThread_ = std::thread([this] { DiscoveryLoop(); });
    else if (warning.empty())
        Notice(L"设备广播不可用，可手动连接设备");
    return true;
}
void Engine::Stop() {
    if (running_) {
        std::vector<Peer> peers;
        { std::lock_guard lock(mutex_); for (auto &p : peers_) if (!p.browser) peers.push_back(p); }
        for (auto &p : peers) { std::string body; RequestPeer(p, "/api/leave", "", body); }
    }
    running_ = false;
    {
        std::lock_guard lock(mutex_);
        for (auto &j : jobs_) {
            j->cancel = true;
            j->cv.notify_all();
        }
    }
    {
        std::lock_guard lock(socketsMutex_);
        for (auto &[s, j] : sockets_)
            shutdown(s, SD_BOTH);
    }
    if (serverThread_.joinable())
        serverThread_.join();
    if (discoveryThread_.joinable())
        discoveryThread_.join();
    std::vector<Worker> workers;
    {
        std::lock_guard lock(workersMutex_);
        workers.swap(workers_);
    }
    for (auto &w : workers)
        if (w.thread.joinable())
            w.thread.join();
    CleanRelay();
    if (listener_ != INVALID_SOCKET) {
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
    }
    if (discovery_ != INVALID_SOCKET) {
        closesocket(discovery_);
        discovery_ = INVALID_SOCKET;
    }
    if (winsock_) {
        WSACleanup();
        winsock_ = false;
    }
}
void Engine::TrackSocket(SOCKET s, const std::shared_ptr<Job> &job) {
    std::lock_guard lock(socketsMutex_);
    sockets_[s] = job;
    if (!running_)
        shutdown(s, SD_BOTH);
}
void Engine::ReleaseSocket(SOCKET s) {
    std::lock_guard lock(socketsMutex_);
    sockets_.erase(s);
    closesocket(s);
}
bool Engine::Launch(std::function<void()> fn) {
    CollectWorkers();
    std::lock_guard lock(workersMutex_);
    if (!running_ || workers_.size() >= 12)
        return false;
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers_.push_back({std::thread([fn = std::move(fn), done] {
                            try {
                                fn();
                            } catch (...) {
                            }
                            done->store(true);
                        }),
                        done});
    return true;
}
void Engine::CollectWorkers() {
    std::lock_guard lock(workersMutex_);
    std::erase_if(workers_, [](Worker &w) {
        if (!w.done->load())
            return false;
        if (w.thread.joinable())
            w.thread.join();
        return true;
    });
}
bool Engine::IsLocal(const std::string &ip) const {
    in_addr addr{};
    if (InetPtonA(AF_INET, ip.c_str(), &addr) != 1)
        return false;
    uint32_t host = ntohl(addr.s_addr);
    if ((host >> 24) == 127)
        return true;
    for (auto &n : networks_)
        if ((host & n.mask) == (n.address & n.mask))
            return true;
    return false;
}
Snapshot Engine::GetSnapshot() {
    std::lock_guard lock(mutex_);
    ExpireLocked();
    Snapshot out;
    out.config = config_;
    out.config.automaticName = config_.name.empty();
    if (out.config.name.empty()) out.config.name = AutoNameLocked(Utf8(config_.id));
    out.ping = ping_;
    out.boundPort = boundPort_;
    out.notice = notice_;
    for (auto &p : peers_) if (p.reachable) out.peers.push_back(p);
    for (auto &n : networks_) {
        out.addresses.push_back(n.text);
        out.interfaces.push_back({n.text, n.name, n.virtualAdapter});
    }
    for (auto &j : jobs_)
        if (!j->hidden) { out.transfers.push_back(j->view); if (!j->recipient.empty()) out.transfers.back().awaitingApproval = false; }
    return out;
}
bool Engine::UpdateConfig(const Config &c, std::wstring &error) {
    if (c.name.size() > 64 || c.name.find_first_of(L"\r\n|") != std::wstring::npos ||
        c.receiveDir.empty() || !c.receiveDir.is_absolute()) {
        error = L"请输入有效的设备名称和绝对接收路径";
        return false;
    }
    try {
        CheckReceiveDirectory(c.receiveDir);
    } catch (const std::exception &exception) {
        error = StorageError(L"接收目录不可写。", c.receiveDir, exception);
        return false;
    }
    try {
        SaveConfig(c);
    } catch (const std::exception &exception) {
        error = StorageError(L"设置保存失败。", c.dataDir / L"settings.ini", exception);
        return false;
    }
    std::vector<uint64_t> webCancel;
    {
        std::lock_guard lock(mutex_);
        if (config_.shareSoftware && !c.shareSoftware)
            for (auto &j : jobs_)
                if ((!j->webClient.empty() || !j->recipient.empty()) &&
                    (j->view.state == State::Queued || j->view.state == State::Waiting ||
                     j->view.state == State::Running))
                    webCancel.push_back(j->view.id);
        config_ = c;
        if (!c.shareSoftware)
            std::erase_if(peers_, [](auto &p) { return p.browser; });
    }
    for (auto id : webCancel)
        Cancel(id);
    refresh_ = true;
    return true;
}
void Engine::Refresh() {
    refresh_ = true;
}
std::string Engine::Advertisement() {
    std::lock_guard lock(mutex_);
    return "CHUAN1|" + Utf8(config_.id) + "|" + std::to_string(boundPort_) + "|" + Hex(Utf8(config_.name));
}
void Engine::DiscoveryLoop() {
    uint64_t last = 0;
    unsigned short discoveryPort;
    {
        std::lock_guard lock(mutex_);
        discoveryPort = config_.discoveryPort;
    }
    auto broadcast = [&] {
        auto message = Advertisement();
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(discoveryPort);
        for (auto &n : networks_) {
            // Bind the announcement sender to each interface; VPN routes must not select its source IP.
            SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sender == INVALID_SOCKET)
                continue;
            BOOL enabled = TRUE;
            setsockopt(sender, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char *>(&enabled), sizeof(enabled));
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = 0;
            local.sin_addr.s_addr = htonl(n.address);
            setsockopt(sender, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&enabled), sizeof(enabled));
            if (bind(sender, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR) {
                closesocket(sender);
                continue;
            }
            to.sin_addr.s_addr = htonl(n.broadcast);
            sendto(sender, message.data(), static_cast<int>(message.size()), 0,
                   reinterpret_cast<sockaddr *>(&to), sizeof(to));
            static constexpr char query[] = "CHUAN_DISCOVER1";
            sendto(discovery_, query, sizeof(query) - 1, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
            closesocket(sender);
        }
        to.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(discovery_, message.data(), static_cast<int>(message.size()), 0,
               reinterpret_cast<sockaddr *>(&to), sizeof(to));
    };
    while (running_) {
        if (Now() - last > 2000 || refresh_.exchange(false)) {
            broadcast();
            last = Now();
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(discovery_, &rd);
        timeval tv{0, 250000};
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0)
            continue;
        char buffer[1200]{};
        sockaddr_in from{};
        int len = sizeof(from);
        int count =
            recvfrom(discovery_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&from), &len);
        if (count <= 0)
            continue;
        char ip[64]{};
        InetNtopA(AF_INET, &from.sin_addr, ip, 64);
        if (!IsLocal(ip))
            continue;
        std::string msg(buffer, count);
        if (msg == "CHUAN_DISCOVER1") {
            auto ad = Advertisement();
            sendto(discovery_, ad.data(), static_cast<int>(ad.size()), 0, reinterpret_cast<sockaddr *>(&from),
                   len);
            continue;
        }
        if (!msg.starts_with("CHUAN1|"))
            continue;
        std::istringstream fields(msg);
        std::string tag, id, port, name;
        std::getline(fields, tag, '|');
        std::getline(fields, id, '|');
        std::getline(fields, port, '|');
        std::getline(fields, name);
        unsigned short p = 0;
        if (id.empty() || id.size() > 64 || !ValidPort(Wide(port), p))
            continue;
        std::wstring decoded = Wide(Unhex(name));
        if (decoded.size() > 64 || decoded.find_first_of(L"\r\n|") != std::wstring::npos)
            continue;
        std::lock_guard lock(mutex_);
        if (Wide(id) == config_.id)
            continue;
        auto it = std::find_if(peers_.begin(), peers_.end(), [&](auto &peer) { return peer.id == Wide(id); });
        Peer peer{Wide(id), decoded.empty() ? AutoNameLocked(id) : decoded, ip, p, Now()};
        peer.reachable = false;
        if (it == peers_.end()) {
            if (peers_.size() < 128)
                peers_.push_back(peer);
        } else {
            peer.manual = it->manual;
            peer.seen = it->seen;
            peer.reachable = it->reachable;
            *it = peer;
        }
    }
}
void Engine::ServerLoop() {
    uint64_t probeAt = 0;
    while (running_) {
        CollectWorkers();
        if (Now() - probeAt > 2000) {
            probeAt = Now();
            CleanRelay();
            std::vector<Peer> manual;
            {
                std::lock_guard lock(mutex_);
                for (auto &p : peers_)
                    if (!p.browser && !probing_.contains(p.id)) {
                        manual.push_back(p); probing_.insert(p.id);
                    }
                ExpireLocked();
            }
            for (auto p : manual)
                if (!Launch([this, p] {
                    std::wstring error;
                    if (!AddPeer(p.ip + ":" + std::to_string(p.port), error)) {
                        std::lock_guard lock(mutex_);
                        std::erase_if(peers_, [&](auto &candidate) {
                            return candidate.id == p.id && candidate.manual && candidate.seen == p.seen &&
                                   Now() - candidate.seen >= 6000;
                        });
                    } else { SyncMesh(p); }
                    { std::lock_guard lock(mutex_); probing_.erase(p.id); }
                })) { std::lock_guard lock(mutex_); probing_.erase(p.id); }
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listener_, &rd);
        timeval tv{0, 250000};
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0)
            continue;
        sockaddr_in address{};
        int len = sizeof(address);
        SOCKET s = accept(listener_, reinterpret_cast<sockaddr *>(&address), &len);
        if (s == INVALID_SOCKET)
            continue;
        char ip[64]{};
        InetNtopA(AF_INET, &address.sin_addr, ip, 64);
        SetTimeout(s);
        if (!IsLocal(ip)) {
            Reply(s, 403, "Forbidden");
            closesocket(s);
            continue;
        }
        TrackSocket(s);
        if (!Launch([this, s, ip = std::string(ip)] {
                try {
                    HandleClient(s, ip);
                } catch (...) {
                    Reply(s, 500, "Internal Server Error");
                }
                // Complete the HTTP response before closing a request whose body
                // was rejected early. A bounded drain prevents a TCP reset from
                // hiding the response in browser clients.
                shutdown(s, SD_SEND);
                const auto drainUntil = Now() + 200;
                std::array<char, 16384> discarded{};
                while (running_ && Now() < drainUntil) {
                    fd_set readable; FD_ZERO(&readable); FD_SET(s, &readable);
                    timeval wait{0, 20000};
                    if (select(0, &readable, nullptr, nullptr, &wait) <= 0 || recv(s, discarded.data(), static_cast<int>(discarded.size()), 0) <= 0) break;
                }
                ReleaseSocket(s);
            })) {
            Reply(s, 503, "Service Unavailable");
            ReleaseSocket(s);
        }
    }
}
void Engine::HandleClient(SOCKET s, const std::string &ip) {
    std::string request;
    std::map<std::string, std::string> headers;
    if (!ReadHeader(s, request, headers)) {
        Reply(s, 400, "Bad Request");
        return;
    }
    std::istringstream line(request);
    std::string method, path, version, extra;
    line >> method >> path >> version;
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        Reply(s, 400, "Bad Request");
        return;
    }
    if (line >> extra) {
        Reply(s, 400, "Bad Request");
        return;
    }
    if (path.starts_with("/api/"))
        ObserveNative(headers, ip);
    if ((path == "/" || path.starts_with("/web/")) && HandleWeb(s, ip, method, path, headers))
        return;
    if (method == "POST" && path == "/api/offer") { ReceiveOffer(s, headers, ip, false); return; }
    if (method == "POST" && path == "/api/upload") {
        ReceiveUpload(s, headers, ip);
        return;
    }
    if (method == "POST" && path == "/api/leave") {
        auto id = Wide(Unhex(Get(headers, "x-chuan-id")));
        std::lock_guard lock(mutex_);
        std::erase_if(peers_, [&](auto &p) { return p.id == id || p.owner == id; });
        Reply(s, 200, "OK"); return;
    }
    if (method == "POST" && path == "/api/ping") {
        auto target = Wide(Unhex(Get(headers, "x-chuan-target")));
        Peer selected; bool found = false;
        {
            std::lock_guard lock(mutex_);
            auto &at = pingTimes_[ip];
            if (Now() - at < 800) { Reply(s, 429, "Too Many Requests"); return; }
            at = Now();
            if (target == config_.id) { ++ping_; Reply(s, 200, "OK"); return; }
            for (auto &p : peers_) if (p.id == target && p.owner.empty()) { selected = p; found = true; }
        }
        std::wstring error;
        bool ok = found && PingNow(selected, error);
        Reply(s, ok ? 200 : 408, ok ? "OK" : "Request Timeout", Utf8(error)); return;
    }
    if (method == "POST" && path == "/api/mesh") {
        uint64_t size = 0;
        if (!Number(Get(headers, "content-length"), size) || size > 1024 * 1024) {
            Reply(s, 400, "Bad Request"); return;
        }
        std::string ledger(static_cast<size_t>(size), '\0');
        if (!ReadExact(s, ledger.data(), ledger.size())) return;
        Reply(s, 200, "OK", Mesh(ledger)); return;
    }
    if (method != "GET") {
        Reply(s, 405, "Method Not Allowed");
        return;
    }
    Config cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = config_;
    }
    if (path == "/api/mesh") {
        Reply(s, 200, "OK", Mesh(Unhex(Get(headers, "x-chuan-ledger")))); return;
    }
    if (path == "/api/info") {
        Reply(s, 200, "OK",
              "{\"protocol\":1,\"id\":" + Json(Utf8(cfg.id)) + ",\"nameHex\":" + Json(Hex(Utf8(cfg.name))) +
                  ",\"port\":" + std::to_string(boundPort_) + "}",
              "application/json; charset=utf-8");
        return;
    }
    if (!cfg.shareSoftware) {
        Reply(s, 403, "Forbidden", Utf8(L"Web服务已关闭"));
        return;
    }
    if (path != "/download/Chuan.exe") {
        Reply(s, 404, "Not Found");
        return;
    }
    WinFile file(CreateFileW(ExePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER size{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size)) {
        Reply(s, 500, "Internal Server Error");
        return;
    }
    if (!SendAll(s, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: "
                    "application/octet-stream\r\nContent-Disposition: attachment; "
                    "filename=\"Chuan.exe\"\r\nX-Content-Type-Options: nosniff\r\nCache-Control: "
                    "no-store\r\nContent-Length: " +
                        std::to_string(size.QuadPart) + "\r\n\r\n"))
        return;
    std::vector<char> buffer(256 * 1024);
    DWORD read = 0;
    while (running_ &&
           ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
        if (!SendAll(s, buffer.data(), read))
            break;
}
void Engine::ObserveNative(const std::map<std::string, std::string> &h, const std::string &ip) {
    auto id = Wide(Unhex(Get(h, "x-chuan-id")));
    auto name = Wide(Unhex(Get(h, "x-chuan-sender")));
    unsigned short port = 0;
    if (id.empty() || id.size() > 64 || id.starts_with(L"web:") || name.size() > 64 ||
        name.find_first_of(L"\r\n|") != std::wstring::npos || !ValidPort(Wide(Get(h, "x-chuan-port")), port))
        return;
    std::lock_guard lock(mutex_);
    if (id == config_.id)
        return;
    Peer peer{id, name.empty() ? AutoNameLocked(Utf8(id)) : name, ip, port, Now(), true};
    auto found = std::find_if(peers_.begin(), peers_.end(), [&](auto &p) { return p.id == id; });
    if (found != peers_.end())
        *found = peer;
    else if (peers_.size() < 128)
        peers_.push_back(peer);
}
bool Engine::HandleWeb(SOCKET s, const std::string &ip, const std::string &method, const std::string &path,
                       const std::map<std::string, std::string> &h) {
    Config cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = config_;
    }
    if (!cfg.shareSoftware) {
        Reply(s, 403, "Forbidden", Utf8(L"Web服务已关闭"));
        return true;
    }
    const std::string host = Get(h, "host"), origin = Get(h, "origin");
    std::string hostIp;
    unsigned short hostPort = 0;
    bool validHost = host == "localhost:" + std::to_string(boundPort_);
    if (Endpoint(host, hostIp, hostPort) && hostPort == boundPort_)
        validHost = hostIp == "127.0.0.1" || std::any_of(networks_.begin(), networks_.end(),
                                                         [&](auto &n) { return n.text == hostIp; });
    if (!validHost || (!origin.empty() && origin != "http://" + host)) {
        Reply(s, 403, "Forbidden", Utf8(L"请使用本机 IP 地址和端口访问 Web服务"));
        return true;
    }
    if (method == "POST") {
        bool validSession = false;
        { std::lock_guard lock(mutex_);
          auto it = browsers_.find(Get(h, "x-chuan-client"));
          validSession = it != browsers_.end() && it->second.token == Get(h, "x-chuan-token"); }
        if (!validSession) {
            Reply(s, 403, "Forbidden", Utf8(L"请求无效，请刷新网页"));
            return true;
        }
        if (path == "/web/api/offer") { ReceiveOffer(s, h, ip, true); return true; }
        if (path.starts_with("/web/api/decision/")) {
            uint64_t id = 0, length = 0; bool owned = false; size_t count = 0;
            if (Number(path.substr(18), id) && Number(Get(h, "content-length"), length) && Get(h, "transfer-encoding").empty()) {
                std::lock_guard lock(mutex_);
                for (auto &j : jobs_) if (j->view.id == id && j->recipient == Get(h, "x-chuan-client") && j->view.awaitingApproval) {
                    owned = true; count = j->view.offerItems.size();
                }
            }
            if (!owned || length != count) { Reply(s, 400, "Bad Request"); return true; }
            std::string selection(count, '0');
            if (!ReadExact(s, selection.data(), selection.size()) || selection.find_first_not_of("01") != std::string::npos) {
                Reply(s, 400, "Bad Request"); return true;
            }
            DecideItems(id, selection); Reply(s, 200, "OK"); return true;
        }
        if (path == "/web/api/upload") {
            ReceiveUpload(s, h, ip, true);
            return true;
        }
        if (Get(h, "content-length") != "0" || !Get(h, "transfer-encoding").empty()) {
            Reply(s, 400, "Bad Request");
            return true;
        }
        if (path == "/web/api/preferences") {
            uint64_t count = 0, bytes = 0;
            if (!Number(Get(h, "x-chuan-max-files"), count) || !Number(Get(h, "x-chuan-max-bytes"), bytes)) { Reply(s, 400, "Bad Request"); return true; }
            std::lock_guard lock(mutex_); auto &session = browsers_.at(Get(h, "x-chuan-client"));
            session.maxFiles = count; session.maxBytes = bytes;
            Reply(s, 200, "OK"); return true;
        }
        if (path == "/web/api/leave") {
            std::lock_guard lock(mutex_);
            auto &session = browsers_.at(Get(h, "x-chuan-client"));
            session.tabs.erase(Get(h, "x-chuan-tab"));
            if (session.tabs.empty()) std::erase_if(peers_, [&](auto &p) { return p.id == session.id; });
            refresh_ = true;
            Reply(s, 200, "OK"); return true;
        }
        if (path == "/web/api/note") {
            auto address = Get(h, "x-chuan-address");
            auto note = Wide(Unhex(Get(h, "x-chuan-note")));
            if (!IsLocal(address) || note.size() > 48 || note.find_first_of(L"\r\n|") != std::wstring::npos) {
                Reply(s, 400, "Bad Request"); return true;
            }
            std::lock_guard lock(mutex_);
            browsers_.at(Get(h, "x-chuan-client")).notes[address] = note;
            Reply(s, 200, "OK"); return true;
        }
        if (path == "/web/api/ping") {
            Peer peer; bool found = false;
            { std::lock_guard lock(mutex_);
              auto &at = pingTimes_[Get(h, "x-chuan-client")];
              if (Now() - at < 800) { Reply(s, 429, "Too Many Requests"); return true; }
              at = Now();
              auto id = Wide(Unhex(Get(h, "x-chuan-peer")));
              if (id == config_.id) { ++ping_; Reply(s, 200, "OK"); return true; }
              for (auto &p : peers_) if (p.id == id && Now() - p.seen < 6000) { peer = p; found = true; }
            }
            std::wstring error;
            bool ok = found && PingNow(peer, error);
            Reply(s, ok ? 200 : 408, ok ? "OK" : "Request Timeout", ok ? "Ping delivered" : Utf8(error));
            return true;
        }
        if (path == "/web/api/refresh") {
            Refresh();
            Reply(s, 200, "OK");
            return true;
        }
        if (path == "/web/api/clear") {
            std::lock_guard lock(mutex_);
            const auto client = Get(h, "x-chuan-client");
            auto busy = BusyFoldersLocked();
            for (auto &j : jobs_) if (j->view.state >= State::Completed && !busy.contains(TransferGroup(j->view)) && (j->webClient == client || j->recipient == client))
                j->hiddenClients.insert(client);
            Reply(s, 200, "OK"); return true;
        }
        if (path.starts_with("/web/api/delete/")) {
            uint64_t id = 0; bool owned = false, recipient = false;
            const auto client = Get(h, "x-chuan-client");
            if (Number(path.substr(16), id)) {
                std::lock_guard lock(mutex_);
                for (auto &j : jobs_) if (j->view.id == id && j->view.state >= State::Completed) {
                    owned = j->webClient == client;
                    recipient = j->recipient == client && !owned;
                    if (recipient) j->hiddenClients.insert(client);
                }
            }
            std::wstring error;
            if (recipient) Reply(s, 200, "OK");
            else if (!owned) Reply(s, 404, "Not Found");
            else if (!DeleteTransfer(id, true, error)) Reply(s, 409, "Conflict", Utf8(error));
            else Reply(s, 200, "OK");
            return true;
        }
        if (path.starts_with("/web/api/cancel/")) {
            auto key = path.substr(16); uint64_t id = 0, number = 0; Number(key, number);
            { std::lock_guard lock(mutex_);
              const auto client = Get(h, "x-chuan-client");
              for (auto &j : jobs_) if ((j->webClient == client || j->recipient == client) &&
                  ((number && j->view.id == number) || (WebKey(key) && j->webUpload == key)) && j->view.state < State::Completed) {
                  id = j->view.id; break;
              } }
            if (id) { Cancel(id); Reply(s, 200, "OK"); }
            else Reply(s, 404, "Not Found", Utf8(L"任务已结束或不存在"));
            return true;
        }
        if (path.starts_with("/web/api/forward/")) {
            uint64_t id = 0;
            if (!Number(path.substr(17), id) || !WebKey(Get(h, "x-chuan-upload"))) {
                Reply(s, 400, "Bad Request");
                return true;
            }
            auto snapshot = GetSnapshot();
            auto peerId = Wide(Unhex(Get(h, "x-chuan-peer")));
            auto peer = std::find_if(snapshot.peers.begin(), snapshot.peers.end(),
                                     [&](auto &p) { return p.id == peerId; });
            if (peer == snapshot.peers.end()) {
                Reply(s, 409, "Conflict", Utf8(L"目标设备不在线"));
                return true;
            }
            WebFile file;
            {
                std::lock_guard lock(mutex_);
                auto it = webFiles_.find(id);
                if (it == webFiles_.end()) {
                    Reply(s, 404, "Not Found");
                    return true;
                }
                file = it->second;
                auto upload = std::find_if(jobs_.begin(), jobs_.end(), [&](auto &j) { return j->view.id == id; });
                if (upload != jobs_.end() && !(*upload)->targetId.empty() && (*upload)->targetId != peerId) {
                    Reply(s, 409, "Conflict", Utf8(L"发送目标与上传登记不一致")); return true;
                }
                for (auto &existing : jobs_) if (existing->webClient == Get(h, "x-chuan-client") &&
                    existing->webUpload == Get(h, "x-chuan-upload") && existing->view.id != id && !existing->view.receiving) {
                    Reply(s, 202, "Accepted"); return true;
                }
                if (file.owner != Get(h, "x-chuan-client")) {
                    Reply(s, 404, "Not Found");
                    return true;
                }
            }
            WinFile source(CreateFileW(file.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            const auto &old = file.identity;
            bool unchanged = file.hasIdentity && source.value != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandle(source.value, &info) && WebHandleMatches(source.value, file.path) &&
                info.dwVolumeSerialNumber == old.dwVolumeSerialNumber &&
                info.nFileIndexHigh == old.nFileIndexHigh && info.nFileIndexLow == old.nFileIndexLow &&
                info.nFileSizeHigh == old.nFileSizeHigh && info.nFileSizeLow == old.nFileSizeLow &&
                CompareFileTime(&info.ftLastWriteTime, &old.ftLastWriteTime) == 0;
            if (!SafeWebPath(file.path, file.root) || !unchanged) {
                Reply(s, 409, "Conflict", Utf8(L"文件已移除或路径已变更"));
                return true;
            }
            std::shared_ptr<Job> job;
            {
                std::lock_guard lock(mutex_);
                // Check and reserve under one lock: concurrent retries share one transfer.
                for (auto &existing : jobs_) if (existing->webClient == Get(h, "x-chuan-client") &&
                    existing->webUpload == Get(h, "x-chuan-upload") && existing->view.id != id && !existing->view.receiving) {
                    Reply(s, 202, "Accepted"); return true;
                }
                job = std::make_shared<Job>();
                job->view.id = nextJob_++;
                job->view.name = file.relative.empty() ? file.path.filename().wstring() : file.relative;
                job->view.peer = peer->name;
                job->view.total = file.size;
                jobs_.push_back(job);
                job->webClient = Get(h, "x-chuan-client");
                job->webUpload = Get(h, "x-chuan-upload");
                for (auto &upload : jobs_) if (upload->view.id == id) {
                    job->senderName = upload->senderName;
                    job->batchId = upload->batchId; job->batchItem = upload->batchItem;
                    job->folderId = upload->folderId;
                    job->view.folderId = upload->view.folderId;
                    job->view.folderName = upload->view.folderName;
                    job->view.folderCount = upload->view.folderCount;
                    job->view.folderTotal = upload->view.folderTotal;
                    break;
                }
                for (auto &upload : jobs_) if (upload->view.id == id && upload->staging) upload->hidden = true;
                job->staging = file.root == (cfg.dataDir / L"relay");
                job->targetId = peerId;
                if (!config_.shareSoftware)
                    job->cancel = true;
            }
            if (peer->browser && peer->owner.empty()) {
                QueueBrowser(*peer, file.path, job, file.root);
                Reply(s, 202, "Accepted");
            } else if (!Launch([this, file, peer = *peer, job] {
                           SendOne(peer, file.path, job->view.name, job, file.root);
                       })) {
                SetState(job, State::Failed, L"当前任务较多，请稍后重试");
                Reply(s, 503, "Service Unavailable");
            } else
                Reply(s, 202, "Accepted", "{\"id\":" + Json(std::to_string(job->view.id)) + "}",
                      "application/json; charset=utf-8");
            return true;
        }
        Reply(s, 404, "Not Found");
        return true;
    }
    if (method != "GET") {
        Reply(s, 405, "Method Not Allowed");
        return true;
    }
    int asset = path == "/"                ? 201
                : path == "/web/style.css" ? 202
                : path == "/web/app.js"    ? 203
                : path == "/web/sha256.js" ? 204
                : path == "/web/files.js" ? 205
                : path == "/web/transfers.js" ? 206
                                           : 0;
    if (asset) {
        auto body = WebResource(asset);
        if (body.empty())
            Reply(s, 500, "Internal Server Error");
        else
            WebReply(s, body,
                     asset == 201   ? "text/html; charset=utf-8"
                     : asset == 202 ? "text/css; charset=utf-8"
                                    : "text/javascript; charset=utf-8");
        return true;
    }
    if (path == "/web/api/state") {
        const auto client = Get(h, "x-chuan-client");
        if (!WebKey(client)) { Reply(s, 401, "Unauthorized"); return true; }
        Peer coordinator; bool newSession = false;
        { std::lock_guard lock(mutex_);
          newSession = !browsers_.contains(client);
          std::wstring leader = config_.id;
          for (auto &p : peers_) if (!p.browser && p.reachable && Now() - p.seen < 6000 && p.id < leader) {
              leader = p.id; coordinator = p;
          } }
        if (newSession && !coordinator.id.empty()) SyncMesh(coordinator);
        BrowserSession session;
        {
            std::lock_guard lock(mutex_);
            auto &current = browsers_[client];
            if (current.id.empty()) { current.id = L"web:" + Guid(); current.token = Hex(Utf8(Guid())); }
            auto name = Wide(Unhex(Get(h, "x-chuan-name")));
            if (name.size() > 48 || name.find_first_of(L"\r\n|") != std::wstring::npos) {
                Reply(s, 400, "Bad Request"); return true;
            }
            current.name = name;
            if (name.empty()) name = AutoNameLocked(Utf8(current.id));
            current.ip = ip;
            current.tabs[Get(h, "x-chuan-tab")] = Now();
            uint64_t ack = 0;
            if (Number(Get(h, "x-chuan-ping-ack"), ack) && ack <= current.ping) current.ack = std::max(current.ack, ack);
            Peer peer{current.id, name, ip, boundPort_, Now(), false, true};
            auto found = std::find_if(peers_.begin(), peers_.end(), [&](auto &p) { return p.id == peer.id; });
            if (found != peers_.end()) *found = peer;
            else { peers_.push_back(peer); refresh_ = true; }
            session = current;
        }
        if (newSession && !coordinator.id.empty()) SyncMesh(coordinator);
        auto snapshot = GetSnapshot();
        const auto clientIp = ip.starts_with("127.") && !snapshot.addresses.empty() ? snapshot.addresses.front() : ip;
        std::ostringstream out;
        out << "{\"name\":" << Json(Utf8(snapshot.config.name)) << ",\"version\":" << Json(Utf8(Version))
            << ",\"autoReceive\":" << (snapshot.config.autoReceive ? "true" : "false")
            << ",\"maxTransferFiles\":" << Json(std::to_string(snapshot.config.maxTransferFiles))
            << ",\"maxTransferBytes\":" << Json(std::to_string(snapshot.config.maxTransferBytes))
            << ",\"hostId\":" << Json(Utf8(snapshot.config.id)) << ",\"token\":" << Json(session.token) << ",\"clientIp\":" << Json(clientIp) << ",\"clientName\":" << Json(Utf8(session.name.empty() ? [&] { std::lock_guard lock(mutex_); return AutoNameLocked(Utf8(session.id)); }() : session.name)) << ",\"ping\":" << (Now() - session.pingAt < 4000 ? session.ping : 0)
            << ",\"hostNote\":" << Json(Utf8(session.notes[hostIp])) << ",\"peers\":[";
        bool comma = false;
        for (auto &p : snapshot.peers) {
            if (p.id == session.id)
                continue;
            if (comma)
                out << ',';
            comma = true;
            out << "{\"id\":" << Json(Utf8(p.id)) << ",\"name\":" << Json(Utf8(p.name))
                << ",\"note\":" << Json(Utf8(session.notes[p.ip])) << ",\"ip\":" << Json(p.ip) << ",\"browser\":" << (p.browser ? "true" : "false") << '}';
        }
        std::map<uint64_t, std::string> owned;
        std::map<uint64_t, std::shared_ptr<Job>> stateJobs;
        std::set<std::string> visibleFolders;
        { std::lock_guard lock(mutex_);
          for (auto &j : jobs_) stateJobs[j->view.id] = j;
          for (auto &j : jobs_) if (j->webClient == client || j->recipient == client) {
              owned[j->view.id] = j->webUpload;
              if (!j->hidden && !j->view.removed && !j->hiddenClients.contains(client)) visibleFolders.insert(TransferGroup(j->view));
          } }
        out << "],\"inbox\":[";
        comma = false;
        {
            std::lock_guard lock(mutex_);
            for (auto &[id, file] : webFiles_) {
                if (file.recipient != client || client.empty() || !file.delivery || file.delivery->hiddenClients.contains(client) ||
                    file.delivery->view.state == State::Cancelled ||
                    file.delivery->view.state == State::Failed)
                    continue;
                if (comma)
                    out << ',';
                comma = true;
                out << "{\"id\":" << Json(std::to_string(id))
                    << ",\"name\":" << Json(Utf8(file.path.filename().wstring())) << ",\"size\":" << file.size
                    << ",\"received\":" << (file.delivery->view.state == State::Completed ? "true" : "false")
                    << '}';
            }
        }
        out << "],\"requests\":[";
        comma = false;
        { std::lock_guard lock(mutex_);
          for (auto &j : jobs_) if (j->recipient == client && j->view.requestOnly && j->view.awaitingApproval) {
              if (comma) out << ','; comma = true;
              out << "{\"id\":" << Json(std::to_string(j->view.id)) << ",\"sender\":" << Json(Utf8(j->view.peer)) << ",\"batch\":" << Json(j->batchId) << ",\"items\":[";
              bool itemComma = false;
              for (auto &item : j->view.offerItems) {
                  if (itemComma) out << ','; itemComma = true;
                  out << "{\"name\":" << Json(Utf8(item.name)) << ",\"folder\":" << (item.folder ? "true" : "false") << ",\"count\":" << Json(std::to_string(item.count)) << ",\"total\":" << Json(std::to_string(item.total)) << '}';
              }
              out << "]}";
          }
        }
        out << "],\"jobs\":[";
        comma = false;
        for (auto it = snapshot.transfers.rbegin(); it != snapshot.transfers.rend(); ++it) {
            auto &j = *it;
            if (j.requestOnly) continue;
            bool receiving = j.receiving;
            std::wstring remote = j.peer;
            bool canDelete = false;
            bool removed = j.removed;
            {
                std::lock_guard lock(mutex_);
                auto found = stateJobs.find(j.id);
                if (found != stateJobs.end()) {
                    auto &job = *found->second;
                    if (job.hiddenClients.contains(client)) { if (j.folderId.empty()) continue; removed = true; }
                    canDelete = !client.empty() && job.webClient == client;
                    if (job.webClient == client && j.receiving) {
                        receiving = false;
                        remote = job.targetId.empty() || job.targetId == snapshot.config.id ? snapshot.config.name : [&] { for (auto &p : snapshot.peers) if (p.id == job.targetId) return p.name; return std::wstring(L"目标设备"); }();
                    } else if (job.recipient == client && !client.empty()) {
                        receiving = true;
                        remote = job.senderName.empty() ? snapshot.config.name : job.senderName;
                    } else if (!client.empty() && job.webClient != client && job.recipient != client)
                        continue;
                } else if (!client.empty())
                    continue;
            }
            if (removed && !visibleFolders.contains(TransferGroup(j))) continue;
            if (comma)
                out << ',';
            comma = true;
            out << "{\"id\":" << Json(std::to_string(j.id)) << ",\"name\":" << Json(Utf8(j.name))
                << ",\"peer\":" << Json(Utf8(remote)) << ",\"total\":" << j.total << ",\"done\":" << j.done
                << ",\"speed\":" << j.speed << ",\"state\":" << static_cast<int>(j.state)
                << ",\"batchId\":" << Json([&] { std::lock_guard lock(mutex_); auto found = stateJobs.find(j.id); return found == stateJobs.end() ? std::string{} : found->second->batchId; }())
                << ",\"folderId\":" << Json(j.folderId) << ",\"folderName\":" << Json(Utf8(j.folderName))
                << ",\"folderCount\":" << j.folderCount << ",\"folderTotal\":" << j.folderTotal
                << ",\"removed\":" << (removed ? "true" : "false")
                << ",\"stateText\":" << Json(Utf8(StateText(j.state, receiving)))
                << ",\"receiving\":" << (receiving ? "true" : "false")
                << ",\"canDelete\":" << (canDelete ? "true" : "false")
                << ",\"fileId\":" << Json(std::to_string(j.id)) << ",\"error\":" << Json(Utf8(j.error))
                << ",\"uploadKey\":" << Json(owned[j.id]) << '}';
        }
        out << "]}";
        Reply(s, 200, "OK", out.str(), "application/json; charset=utf-8");
        return true;
    }
    if (path.starts_with("/web/files/")) {
        uint64_t id = 0;
        auto query = path.find('?');
        auto client = Get(h, "x-chuan-client");
        if (query != std::string::npos && path.substr(query + 1).starts_with("client="))
            client = path.substr(query + 8);
        if (!Number(path.substr(11, query == std::string::npos ? std::string::npos : query - 11), id))
            Reply(s, 404, "Not Found");
        else
            WebDownload(s, ip, id, client);
        return true;
    }
    Reply(s, 404, "Not Found");
    return true;
}
void Engine::WebDownload(SOCKET s, const std::string &ip, uint64_t id, const std::string &client) {
    WebFile entry;
    {
        std::lock_guard lock(mutex_);
        auto found = webFiles_.find(id);
        if (found == webFiles_.end()) {
            Reply(s, 404, "Not Found");
            return;
        }
        entry = found->second;
        if (client.empty() || (entry.recipient != client && entry.owner != client)) {
            Reply(s, 404, "Not Found");
            return;
        }
    }
    if (!SafeWebPath(entry.path, entry.root)) {
        Reply(s, 404, "Not Found");
        return;
    }
    WinFile file(CreateFileW(entry.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    LARGE_INTEGER size{};
    BY_HANDLE_FILE_INFORMATION info{};
    const auto &old = entry.identity;
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size) ||
        !entry.hasIdentity || !GetFileInformationByHandle(file.value, &info) ||
        info.dwVolumeSerialNumber != old.dwVolumeSerialNumber ||
        info.nFileIndexHigh != old.nFileIndexHigh || info.nFileIndexLow != old.nFileIndexLow ||
        CompareFileTime(&info.ftLastWriteTime, &old.ftLastWriteTime) != 0 ||
        !WebHandleMatches(file.value, entry.path) || static_cast<uint64_t>(size.QuadPart) != entry.size) {
        Reply(s, 409, "Conflict", Utf8(L"文件已移除或发生变更"));
        return;
    }
    auto job = entry.delivery;
    if (job) {
        std::lock_guard lock(mutex_);
        if (job->view.state == State::Running || job->cancel) {
            Reply(s, 409, "Conflict");
            return;
        }
        job->view.state = State::Running;
        job->view.done = 0;
        job->view.started = Now();
    } else
        job = NewJob(entry.path.filename().wstring(), L"Web 浏览器 (" + Wide(ip) + L")", entry.size, false);
    RememberFile(job, entry.path);
    {
        std::lock_guard lock(mutex_);
        if (job->webClient.empty() && job->recipient.empty())
            job->webClient = client.empty() ? "download" : client;
        if (!config_.shareSoftware)
            job->cancel = true;
    }
    if (job->cancel) {
        SetState(job, State::Cancelled);
        Reply(s, 403, "Forbidden");
        return;
    }
    TrackSocket(s, job);
    SetState(job, State::Running);
    bool ok =
        SendAll(s, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n"
                   "Content-Disposition: attachment; filename=\"download\"; filename*=UTF-8''" +
                       PercentEncode(Utf8(entry.path.filename().wstring())) +
                       "\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\nContent-Length: " +
                       std::to_string(entry.size) + "\r\n\r\n");
    std::vector<char> buffer(256 * 1024);
    uint64_t done = 0;
    while (ok && running_ && !job->cancel && done < entry.size) {
        DWORD read = 0;
        ok = ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read &&
             SendAll(s, buffer.data(), read);
        if (ok) {
            done += read;
            Progress(job, done);
        }
    }
    SetState(job, job->cancel || !running_   ? State::Cancelled
                  : ok && done == entry.size ? State::Completed
                                             : State::Failed);
}
std::shared_ptr<Engine::Job> Engine::NewJob(const std::wstring &name, const std::wstring &peer,
                                            uint64_t total, bool receiving) {
    std::lock_guard lock(mutex_);
    auto j = std::make_shared<Job>();
    j->view.id = nextJob_++;
    j->view.name = name;
    j->view.peer = peer;
    j->view.total = total;
    j->view.receiving = receiving;
    jobs_.push_back(j);
    return j;
}
void Engine::SetState(const std::shared_ptr<Job> &j, State s, const std::wstring &error) {
    std::lock_guard lock(mutex_);
    j->view.state = s;
    j->view.error = error;
    if (s >= State::Completed) { j->finishedAt = Now(); j->view.awaitingApproval = false; }
    j->cv.notify_all();
    if (s == State::Running && !j->view.started)
        j->view.started = Now();
}
void Engine::Progress(const std::shared_ptr<Job> &j, uint64_t done) {
    std::lock_guard lock(mutex_);
    j->view.done = done;
    uint64_t ms = Now() - j->view.started;
    if (ms)
        j->view.speed = static_cast<double>(done) * 1000 / static_cast<double>(ms);
}
void Engine::RememberFile(const std::shared_ptr<Job> &job, const fs::path &path) {
    WinFile file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    bool valid = file.value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file.value, &info) &&
                 !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    std::lock_guard lock(mutex_);
    job->view.savedPath = fs::absolute(path).lexically_normal();
    job->hasIdentity = valid;
    job->fileIdentity = info;
}
void Engine::QueueBrowser(const Peer &peer, const fs::path &path, const std::shared_ptr<Job> &job,
                          const fs::path &root) {
    RememberFile(job, path);
    std::lock_guard lock(mutex_);
    auto online = std::find_if(peers_.begin(), peers_.end(), [&](auto &p) {
        return p.browser && p.id == peer.id && Now() - p.seen < 6000;
    });
    if (!config_.shareSoftware || online == peers_.end() || job->cancel || !job->hasIdentity) {
        job->view.state = State::Failed;
        job->view.error = L"浏览器设备已离线或文件不可读";
        return;
    }
    auto browser = std::find_if(browsers_.begin(), browsers_.end(), [&](auto &entry) { return entry.second.id == peer.id; });
    if (browser == browsers_.end()) { job->view.state = State::Failed; return; }
    if ((browser->second.maxFiles && (job->view.folderCount ? job->view.folderCount : 1) > browser->second.maxFiles) ||
        (browser->second.maxBytes && (job->view.folderCount ? job->view.folderTotal : job->view.total) > browser->second.maxBytes)) {
        job->view.state = State::Rejected; job->view.error = L"超过浏览器设置的传输上限"; job->finishedAt = Now(); return;
    }
    job->recipient = browser->first;
    if (job->senderName.empty()) job->senderName = job->view.receiving ? job->view.peer :
        (config_.name.empty() ? AutoNameLocked(Utf8(config_.id)) : config_.name);
    if (!job->webClient.empty()) {
        auto sender = browsers_.find(job->webClient);
        if (sender != browsers_.end()) job->senderName = sender->second.name.empty() ?
            AutoNameLocked(Utf8(sender->second.id)) : sender->second.name;
    }
    job->view.state = State::Waiting;
    job->view.done = 0; job->view.speed = 0; job->view.started = 0;
    auto absolute = job->view.savedPath;
    webFiles_[job->view.id] = {absolute, root.empty() ? absolute.parent_path() : root, job->view.total,
                               job->recipient, job, {}, job->fileIdentity, job->hasIdentity, job->view.name};
}
void Engine::ReceiveUpload(SOCKET s, const std::map<std::string, std::string> &h, const std::string &ip,
                           bool browser) {
    const auto name = SanitizeRelative(Wide(Unhex(Get(h, "x-chuan-path"))));
    auto sender = Wide(Unhex(Get(h, Get(h, "x-chuan-display-sender").empty() ? "x-chuan-sender" : "x-chuan-display-sender")));
    uint64_t total = 0, length = 0;
    if (name.empty() || sender.empty() || sender.size() > 64 ||
        sender.find_first_of(L"\r\n") != std::wstring::npos || !Number(Get(h, "x-chuan-size"), total) ||
        !Number(Get(h, "content-length"), length) ||
        total > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 32 || length != total + 32 ||
        (!browser && Get(h, "expect") != "100-continue") || !Get(h, "transfer-encoding").empty() ||
        (browser && (total > 9007199254740991ull || !Get(h, "expect").empty() ||
                     !WebKey(Get(h, "x-chuan-client")) || !WebKey(Get(h, "x-chuan-upload"))))) {
        Reply(s, 400, "Bad Request");
        return;
    }
    Config cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = config_;
    }
    if (cfg.maxTransferBytes && total > cfg.maxTransferBytes) { Reply(s, 413, "Content Too Large", Utf8(L"超过接收端设置的传输大小上限")); return; }
    const auto target = Wide(Unhex(Get(h, "x-chuan-target")));
    const auto recipientId = Wide(Unhex(Get(h, "x-chuan-recipient")));
    Peer browserPeer;
    if (!recipientId.empty()) {
        auto snapshot = GetSnapshot();
        auto found = std::find_if(snapshot.peers.begin(), snapshot.peers.end(), [&](auto &p) { return p.id == recipientId && p.browser && p.owner.empty(); });
        if (browser || found == snapshot.peers.end() || !cfg.shareSoftware) { Reply(s, 409, "Conflict"); return; }
        browserPeer = *found;
        std::lock_guard lock(mutex_);
        for (auto &[key, session] : browsers_) if (session.id == recipientId && session.maxBytes && total > session.maxBytes) {
            Reply(s, 413, "Content Too Large", Utf8(L"超过浏览器设置的传输上限")); return;
        }
    }
    bool staging = browser && !target.empty() && target != cfg.id;
    if (staging) {
        auto snapshot = GetSnapshot();
        if (std::none_of(snapshot.peers.begin(), snapshot.peers.end(), [&](auto &p) { return p.id == target; })) {
            Reply(s, 409, "Conflict", Utf8(L"目标设备不在线")); return;
        }
    }
    if (staging || !recipientId.empty()) {
        cfg.receiveDir = cfg.dataDir / L"relay";
        std::error_code ec; fs::create_directories(cfg.receiveDir, ec);
    }
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(cfg.receiveDir.c_str(), &available, nullptr, nullptr) ||
        total > available.QuadPart || available.QuadPart - total < 2 * 1024 * 1024) {
        Reply(s, 507, "Insufficient Storage");
        return;
    }
    auto j = NewJob(name, sender + L" (" + Wide(ip) + L")", total, true);
    j->staging = staging || !recipientId.empty();
    j->targetId = target; j->senderName = sender;
    if (browser) {
        std::lock_guard lock(mutex_);
        j->staging = staging;
        j->webClient = Get(h, "x-chuan-client");
        j->webUpload = Get(h, "x-chuan-upload");
        if (!config_.shareSoftware)
            j->cancel = true;
    }
    TrackSocket(s, j);
    const auto batchId = Get(h, "x-chuan-batch");
    if (!batchId.empty()) {
        uint64_t index = 0; bool valid = false;
        const auto key = (browser ? "web:" : "native:") + ip + ":" + Get(h, browser ? "x-chuan-client" : "x-chuan-id") + ":" + batchId;
        { std::lock_guard lock(mutex_);
          auto found = batchApprovals_.find(key);
          if (WebKey(batchId) && Number(Get(h, "x-chuan-item"), index) && found != batchApprovals_.end()) {
              auto &approval = found->second;
              if (index < approval.items.size() && approval.selection[index] == '1' && approval.target == Get(h, "x-chuan-target") && approval.recipient == Get(h, "x-chuan-recipient")) {
                  auto &item = approval.items[static_cast<size_t>(index)];
                  valid = (item.folder ? name.starts_with(item.name + L"\\") : name == item.name) &&
                      approval.counts[index] < item.count && total <= item.total - approval.bytes[index] &&
                      !approval.paths.contains({static_cast<size_t>(index), name});
                  if (approval.counts[index] + 1 == item.count) valid = valid && approval.bytes[index] + total == item.total;
                  if (item.folder) valid = valid && Wide(Unhex(Get(h, "x-chuan-folder-name"))) == item.name &&
                      Get(h, "x-chuan-folder-count") == std::to_string(item.count) && Get(h, "x-chuan-folder-size") == std::to_string(item.total);
                  else valid = valid && Get(h, "x-chuan-folder").empty() && total == item.total;
                  if (valid) {
                      ++approval.counts[index]; approval.bytes[index] += total; approval.touched = Now();
                      approval.paths.insert({static_cast<size_t>(index), name});
                      j->batchId = batchId; j->batchItem = static_cast<size_t>(index); j->decision = 1;
                  }
              }
          }
        }
        if (!valid) { SetState(j, State::Rejected, L"文件不在已批准的批次范围内"); Reply(s, 417, "Expectation Failed"); return; }
    }
    const auto folderId = Get(h, "x-chuan-folder");
    if (!folderId.empty()) {
        auto folderName = SanitizeRelative(Wide(Unhex(Get(h, "x-chuan-folder-name"))));
        uint64_t count = 0, bytes = 0;
        const auto owner = browser ? Get(h, "x-chuan-client") : Get(h, "x-chuan-id");
        if (!WebKey(folderId) || owner.empty() || folderName.empty() || folderName.find_first_of(L"/\\") != std::wstring::npos ||
            !name.starts_with(folderName + L"\\") || !Number(Get(h, "x-chuan-folder-count"), count) || !count ||
            !Number(Get(h, "x-chuan-folder-size"), bytes) || total > bytes) {
            SetState(j, State::Failed, L"文件夹批次信息无效"); Reply(s, 400, "Bad Request"); return;
        }
        if ((cfg.maxTransferFiles && count > cfg.maxTransferFiles) || (cfg.maxTransferBytes && bytes > cfg.maxTransferBytes)) {
            SetState(j, State::Failed, L"超过接收端设置的单批文件数量或总大小上限"); Reply(s, 413, "Content Too Large", Utf8(L"超过接收端设置的传输上限")); return;
        }
        const std::string key = (browser ? "web:" : "native:") + ip + ":" + owner + ":" + folderId;
        bool valid = true, rejected = false;
        {
            std::lock_guard lock(mutex_);
            auto found = folderApprovals_.find(key);
            if (found == folderApprovals_.end() && folderApprovals_.size() >= 4096) valid = false;
            if (valid) {
                auto [entry, inserted] = folderApprovals_.try_emplace(key);
                auto &batch = entry->second;
                if (inserted) { batch.name = folderName; batch.count = count; batch.total = bytes; }
                valid = batch.name == folderName && batch.count == count && batch.total == bytes &&
                    batch.paths.size() < count && total <= bytes - batch.reserved && !batch.paths.contains(name);
                if (valid) {
                    batch.paths.insert(name); batch.reserved += total;
                    if (cfg.autoReceive || staging || !recipientId.empty()) { if (!batch.decision) batch.decision = 1; }
                    j->folderId = folderId; j->folderKey = key; j->view.folderId = folderId;
                    j->view.folderName = folderName; j->view.folderCount = count; j->view.folderTotal = bytes;
                    if (!j->decision) j->decision = batch.decision;
                    rejected = batch.decision < 0;
                }
            }
        }
        if (!valid) { SetState(j, State::Failed, L"文件夹批次范围不一致或已结束"); Reply(s, 409, "Conflict"); return; }
        if (rejected) { SetState(j, State::Rejected, L"文件夹接收已拒绝"); Reply(s, 417, "Expectation Failed"); return; }
    }
    SetState(j, State::Waiting);
    if (!cfg.autoReceive && !staging && recipientId.empty()) {
        std::unique_lock lock(mutex_);
        j->view.awaitingApproval = j->decision == 0;
        if (!j->folderKey.empty() && !j->decision) {
            auto &batch = folderApprovals_.at(j->folderKey);
            if (!batch.prompt) batch.prompt = j->view.id;
            j->view.awaitingApproval = batch.prompt == j->view.id;
        }
        const uint64_t deadline = Now() + 120000;
        while (j->decision == 0 && !j->cancel && running_ && Now() < deadline) {
            j->cv.wait_for(lock, std::chrono::milliseconds(250));
            if (j->decision != 0 || j->cancel || !running_)
                break;
            lock.unlock();
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(s, &readable);
            timeval immediate{0, 0};
            char byte = 0;
            bool closed =
                select(0, &readable, nullptr, nullptr, &immediate) > 0 && recv(s, &byte, 1, MSG_PEEK) <= 0;
            lock.lock();
            if (closed)
                j->cancel = true;
        }
        if (j->decision != 1 || j->cancel || !running_) {
            if (!j->folderKey.empty() && folderApprovals_.at(j->folderKey).decision == 0) {
                folderApprovals_.at(j->folderKey).decision = -1;
                for (auto &other : jobs_) if (other->folderKey == j->folderKey) { other->decision = -1; other->view.awaitingApproval = false; other->cv.notify_all(); }
            }
            j->view.state = j->cancel ? State::Cancelled : State::Rejected;
            j->view.awaitingApproval = false;
            lock.unlock();
            Reply(s, 417, "Expectation Failed", Utf8(L"接收请求已拒绝或超时"));
            return;
        }
    }
    { std::lock_guard lock(mutex_); j->view.awaitingApproval = false; }
    if (j->cancel) {
        SetState(j, State::Cancelled);
        Reply(s, 417, "Expectation Failed");
        return;
    }
    fs::path destination = cfg.receiveDir / fs::path(name),
             partial = cfg.receiveDir / (L".chuan-" + Guid() + L".part");
    try {
        if (!SafeParents(cfg.receiveDir, destination.parent_path()))
            throw std::runtime_error("unsafe path");
        {
            WinFile file(CreateFileW(
                partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (file.value == INVALID_HANDLE_VALUE)
                throw std::runtime_error("create file");
            Hash hash;
            if (!browser && !SendAll(s, "HTTP/1.1 100 Continue\r\n\r\n"))
                throw std::runtime_error("connection");
            SetState(j, State::Running);
            std::vector<char> buffer(256 * 1024);
            uint64_t done = 0;
            while (done < total) {
                if (j->cancel || !running_)
                    throw std::runtime_error("cancel");
                size_t want = static_cast<size_t>(std::min<uint64_t>(buffer.size(), total - done));
                int n = recv(s, buffer.data(), static_cast<int>(want), 0);
                if (n <= 0)
                    throw std::runtime_error("connection");
                DWORD written = 0;
                if (!WriteFile(file.value, buffer.data(), static_cast<DWORD>(n), &written, nullptr) ||
                    written != static_cast<DWORD>(n))
                    throw std::runtime_error("disk");
                hash.Add(buffer.data(), n);
                done += static_cast<uint64_t>(n);
                Progress(j, done);
            }
            std::array<unsigned char, 32> expected{};
            if (!ReadExact(s, expected.data(), expected.size()) || hash.Finish() != expected)
                throw std::runtime_error("checksum");
            if (j->cancel || !running_)
                throw std::runtime_error("cancel");
            if (!FlushFileBuffers(file.value))
                throw std::runtime_error("flush");
        }
        fs::path chosen;
        for (unsigned i = 0; i < 10000; ++i) {
            chosen = UniquePath(destination, i);
            if (MoveFileExW(partial.c_str(), chosen.c_str(), MOVEFILE_WRITE_THROUGH))
                break;
            DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
                throw std::runtime_error("rename");
            chosen.clear();
        }
        if (chosen.empty())
            throw std::runtime_error("duplicates");
        RememberFile(j, chosen);
        {
            std::lock_guard lock(mutex_);
            j->view.savedPath = chosen;
            if (browser) webFiles_[j->view.id] = {chosen, cfg.receiveDir, total, {}, {}, Get(h, "x-chuan-client"), j->fileIdentity, j->hasIdentity, name};
        }
        if (!recipientId.empty()) {
            QueueBrowser(browserPeer, chosen, j, cfg.receiveDir);
            SendAll(s, "HTTP/1.1 102 Processing\r\n\r\n");
            uint64_t deadline = Now() + 120000, keepalive = Now();
            std::unique_lock lock(mutex_);
            while (running_ && !j->cancel && j->view.state < State::Completed) {
                if (j->view.state == State::Waiting && Now() >= deadline) { j->cancel = true; break; }
                j->cv.wait_for(lock, std::chrono::milliseconds(250));
                if (Now() - keepalive > 2000) {
                    lock.unlock(); bool live = SendAll(s, "HTTP/1.1 102 Processing\r\n\r\n"); lock.lock();
                    keepalive = Now(); if (!live) { j->cancel = true; break; }
                }
            }
            bool complete = j->view.state == State::Completed;
            if (!complete) { j->view.state = State::Cancelled; j->finishedAt = Now(); }
            lock.unlock(); Reply(s, complete ? 201 : 409, complete ? "Created" : "Conflict"); return;
        }
        SetState(j, State::Completed);
        if (browser)
            Reply(s, 201, "Created", "{\"id\":" + Json(std::to_string(j->view.id)) + "}",
                  "application/json; charset=utf-8");
        else
            Reply(s, 201, "Created", Utf8(chosen.filename().wstring()));
    } catch (const std::exception &ex) {
        DeleteFileW(partial.c_str());
        std::wstring reason = std::string(ex.what()) == "checksum" ? L"完整性校验失败"
                              : std::string(ex.what()) == "disk"   ? L"写入接收目录失败"
                                                                   : L"传输中断，未保存正式文件";
        SetState(j, j->cancel || !running_ ? State::Cancelled : State::Failed, reason);
        Reply(s, 422, "Unprocessable Content", Utf8(reason));
    }
}
void Engine::SendOne(const Peer &peer, const fs::path &path, const std::wstring &relative,
                     const std::shared_ptr<Job> &j, const fs::path &webRoot) {
    if (peer.browser && peer.owner.empty()) {
        QueueBrowser(peer, path, j, webRoot);
        return;
    }
    if (j->cancel || !running_) {
        SetState(j, State::Cancelled);
        return;
    }
    WinFile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER size{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size)) {
        SetState(j, State::Failed, L"无法读取文件，文件可能正在被修改");
        return;
    }
    if (!webRoot.empty() && (!SafeWebPath(path, webRoot) || !WebHandleMatches(file.value, path) ||
                             static_cast<uint64_t>(size.QuadPart) != j->view.total)) {
        SetState(j, State::Failed, L"文件路径已变更，无法转发");
        return;
    }
    uint64_t total = static_cast<uint64_t>(size.QuadPart);
    RememberFile(j, path);
    if (total > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 32) {
        SetState(j, State::Failed, L"文件过大");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        j->view.total = total;
    }
    SOCKET s = Connect(peer.gateway.empty() ? peer.ip : peer.gateway, peer.port);
    if (s == INVALID_SOCKET) {
        SetState(j, State::Failed, L"无法连接目标设备");
        return;
    }
    TrackSocket(s, j);
    SetTimeout(s, 150000, 30000);
    try {
        SetState(j, State::Waiting);
        std::wstring sender, senderId;
        {
            std::lock_guard lock(mutex_);
            sender = config_.name.empty() ? AutoNameLocked(Utf8(config_.id)) : config_.name;
            senderId = config_.id;
        }
        std::string request =
            "POST /api/upload HTTP/1.1\r\nHost: " + peer.ip + ":" + std::to_string(peer.port) +
            "\r\nConnection: close\r\nExpect: 100-continue\r\nX-Chuan-Id: " + Hex(Utf8(senderId)) +
            "\r\nX-Chuan-Port: " + std::to_string(boundPort_) + "\r\nX-Chuan-Path: " + Hex(Utf8(relative)) +
            "\r\nX-Chuan-Sender: " + Hex(Utf8(sender)) + "\r\nX-Chuan-Size: " + std::to_string(total) +
            "\r\nX-Chuan-Recipient: " + (peer.browser ? Hex(Utf8(peer.id)) : "") +
            "\r\nX-Chuan-Display-Sender: " + Hex(Utf8(j->senderName)) +
            "\r\nX-Chuan-Batch: " + j->batchId + "\r\nX-Chuan-Item: " + std::to_string(j->batchItem) +
            "\r\nX-Chuan-Folder: " + j->folderId + "\r\nX-Chuan-Folder-Name: " + Hex(Utf8(j->view.folderName)) +
            "\r\nX-Chuan-Folder-Count: " + std::to_string(j->view.folderCount) + "\r\nX-Chuan-Folder-Size: " + std::to_string(j->view.folderTotal) +
            "\r\nContent-Length: " + std::to_string(total + 32) + "\r\n\r\n";
        if (!SendAll(s, request))
            throw std::runtime_error("request");
        std::string first;
        std::map<std::string, std::string> headers;
        if (!ReadHeader(s, first, headers))
            throw std::runtime_error("response");
        if (!first.starts_with("HTTP/1.1 100 ")) {
            SetState(j, first.starts_with("HTTP/1.1 417") ? State::Rejected : State::Failed,
                     first.starts_with("HTTP/1.1 507") ? L"目标设备磁盘空间不足"
                                                       : L"目标设备拒绝接收或服务繁忙");
            ReleaseSocket(s);
            return;
        }
        SetTimeout(s);
        Hash hash;
        SetState(j, State::Running);
        std::vector<char> buffer(256 * 1024);
        uint64_t done = 0;
        while (done < total) {
            if (j->cancel || !running_)
                throw std::runtime_error("cancel");
            DWORD count = 0;
            if (!ReadFile(file.value, buffer.data(),
                          static_cast<DWORD>(std::min<uint64_t>(buffer.size(), total - done)), &count,
                          nullptr) ||
                !count)
                throw std::runtime_error("file");
            hash.Add(buffer.data(), count);
            if (!SendAll(s, buffer.data(), count))
                throw std::runtime_error("send");
            done += count;
            Progress(j, done);
        }
        auto digest = hash.Finish();
        if (!SendAll(s, digest.data(), digest.size()))
            throw std::runtime_error("send");
        headers.clear();
        if (!ReadHeader(s, first, headers)) throw std::runtime_error("confirmation");
        while (first.starts_with("HTTP/1.1 102 ")) {
            SetState(j, State::Waiting); headers.clear();
            if (j->cancel || !ReadHeader(s, first, headers)) throw std::runtime_error("confirmation");
        }
        if (!first.starts_with("HTTP/1.1 201 "))
            throw std::runtime_error("confirmation");
        SetState(j, State::Completed);
    } catch (...) {
        SetState(j, j->cancel || !running_ ? State::Cancelled : State::Failed,
                 L"连接中断或目标设备未确认保存");
    }
    ReleaseSocket(s);
}
void Engine::SendFiles(const Peer &peer, const std::vector<fs::path> &input) {
    if (!running_ || !IsLocal(peer.ip)) {
        Notice(L"目标设备必须位于当前局域网");
        return;
    }
    {
        auto snapshot = GetSnapshot();
        if (std::none_of(snapshot.peers.begin(), snapshot.peers.end(), [&](auto &p) { return p.id == peer.id; })) {
            Notice(L"目标设备已离线，请重新选择在线设备"); return;
        }
    }
    struct Item {
        fs::path path;
        std::wstring relative;
        std::shared_ptr<Job> job;
    };
    std::vector<Item> files;
    std::vector<OfferItem> manifest;
    Config cfg; { std::lock_guard lock(mutex_); cfg = config_; }
    uint64_t batchBytes = 0;
    try {
        for (auto &source : input) {
            if (fs::is_symlink(source)) {
                Notice(L"不支持发送符号链接");
                return;
            }
            if (fs::is_regular_file(source)) {
                auto rel = SanitizeRelative(source.filename().wstring());
                if (rel.empty())
                    throw std::runtime_error("name");
                auto job = NewJob(rel, peer.name, fs::file_size(source), false);
                job->batchItem = manifest.size(); manifest.push_back({rel, false, 1, job->view.total});
                files.push_back({source, rel, job});
            } else if (fs::is_directory(source)) {
                size_t before = files.size();
                for (auto &entry : fs::recursive_directory_iterator(source)) {
                    if (entry.is_symlink())
                        continue;
                    if (entry.is_regular_file()) {
                        auto relative = SanitizeRelative(
                            (source.filename() / entry.path().lexically_relative(source)).wstring());
                        if (relative.empty())
                            throw std::runtime_error("name");
                        files.push_back(
                            {entry.path(), relative, NewJob(relative, peer.name, entry.file_size(), false)});
                    }
                }
                if (before == files.size())
                    Notice(L"空文件夹没有可传输文件");
                else {
                    GUID id{}; CoCreateGuid(&id);
                    const auto folderId = Hex(std::string(reinterpret_cast<const char *>(&id), sizeof(id)));
                    uint64_t bytes = 0;
                    for (size_t i = before; i < files.size(); ++i) { if (files[i].job->view.total > UINT64_MAX - bytes) throw std::runtime_error("overflow"); bytes += files[i].job->view.total; }
                    const size_t itemIndex = manifest.size(); manifest.push_back({source.filename().wstring(), true, files.size() - before, bytes});
                    std::lock_guard lock(mutex_);
                    for (size_t i = before; i < files.size(); ++i) {
                        auto &job = files[i].job;
                        job->batchItem = itemIndex;
                        job->folderId = folderId; job->view.folderId = folderId; job->view.folderName = source.filename().wstring();
                        job->view.folderCount = files.size() - before; job->view.folderTotal = bytes;
                    }
                }
            } else
                throw std::runtime_error("missing");
            if (cfg.maxTransferFiles && files.size() > cfg.maxTransferFiles) throw std::runtime_error("limit");
        }
        for (auto &item : manifest) {
            if (item.total > UINT64_MAX - batchBytes) throw std::runtime_error("overflow");
            batchBytes += item.total;
        }
        if (cfg.maxTransferBytes && batchBytes > cfg.maxTransferBytes) throw std::runtime_error("limit");
    } catch (const std::exception &error) {
        const auto reason = std::string(error.what()) == "limit" ? L"超过设置的单批文件数量或总大小上限" : L"文件列表读取失败，检查文件名、权限或总大小是否可表示";
        for (auto &f : files) SetState(f.job, State::Failed, reason);
        Notice(reason);
        return;
    }
    if (files.empty())
        return;
    for (auto &file : files)
        RememberFile(file.job, file.path);
    auto pending = files;
    bool ok = Launch([this, peer, files = std::move(files), manifest = std::move(manifest)] {
        if (manifest.size() > 1 || manifest[0].folder) {
            GUID value{}; CoCreateGuid(&value); const auto batch = Hex(std::string(reinterpret_cast<const char *>(&value), sizeof(value)));
            std::string selection; std::wstring error;
            for (auto &file : files) SetState(file.job, State::Waiting);
            if (!SendOffer(peer, batch, manifest, selection, error, files[0].job)) {
                for (auto &file : files) SetState(file.job, file.job->cancel ? State::Cancelled : State::Failed, error);
                return;
            }
            for (auto &file : files) {
                { std::lock_guard lock(mutex_); file.job->batchId = batch; }
                if (selection[file.job->batchItem] != '1') SetState(file.job, State::Rejected, L"接收方未选择此项");
            }
        }
        std::set<std::string> stopped;
        for (auto &f : files) {
            { std::lock_guard lock(mutex_); if (f.job->view.state == State::Rejected) continue; }
            if (!f.job->folderId.empty() && stopped.contains(f.job->folderId)) {
                SetState(f.job, State::Cancelled, L"文件夹批次已停止"); continue;
            }
            SendOne(peer, f.path, f.relative, f.job);
            std::lock_guard lock(mutex_);
            if (!f.job->folderId.empty() && f.job->view.state >= State::Cancelled) stopped.insert(f.job->folderId);
        }
    });
    if (!ok) {
        for (auto &f : pending)
            SetState(f.job, State::Failed, L"服务繁忙");
        Notice(L"当前任务过多，请稍后发送");
    }
}
void Engine::Cancel(uint64_t id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard lock(mutex_);
        for (auto &j : jobs_)
            if (j->view.id == id) {
                job = j;
                break;
            }
        if (!job)
            return;
        job->cancel = true;
        job->cv.notify_all();
        if (job->view.state == State::Queued ||
            (!job->recipient.empty() && job->view.state == State::Waiting))
            job->view.state = State::Cancelled;
        job->finishedAt = Now();
    }
    {
        std::lock_guard lock(socketsMutex_);
        for (auto &[s, j] : sockets_)
            if (j.lock() == job)
                shutdown(s, SD_BOTH);
    }
}
void Engine::Decide(uint64_t id, bool accept) {
    std::lock_guard lock(mutex_);
    for (auto &j : jobs_)
        if (j->view.id == id && j->view.receiving && j->view.state == State::Waiting) {
            j->selection.assign(j->view.offerItems.size(), accept ? '1' : '0');
            j->decision = accept ? 1 : -1;
            j->view.awaitingApproval = false;
            if (!j->folderKey.empty()) {
                folderApprovals_.at(j->folderKey).decision = j->decision;
                for (auto &other : jobs_) if (other->folderKey == j->folderKey) {
                    other->decision = j->decision; other->view.awaitingApproval = false; other->cv.notify_all();
                }
            }
            j->cv.notify_all();
        }
}
bool Engine::HasActiveTransfers() {
    std::lock_guard lock(mutex_);
    return std::any_of(jobs_.begin(), jobs_.end(), [](auto &j) {
        return j->view.state == State::Queued || j->view.state == State::Waiting ||
               j->view.state == State::Running;
    });
}
std::set<std::string> Engine::BusyFoldersLocked() {
    std::vector<Transfer> views;
    for (auto &job : jobs_) if (!job->hidden) views.push_back(job->view);
    std::set<std::string> busy;
    for (auto &row : TransferRows(views, {})) if (row.folderRow && row.state < State::Completed) busy.insert(row.group);
    return busy;
}
void Engine::ClearFinished() {
    std::lock_guard lock(mutex_);
    auto busy = BusyFoldersLocked();
    std::erase_if(jobs_, [&](auto &j) {
        if (busy.contains(TransferGroup(j->view))) return false;
        if (j->staging) { if (j->view.state >= State::Completed) j->hidden = true; return false; }
        return j->view.state != State::Queued && j->view.state != State::Waiting &&
               j->view.state != State::Running;
    });
}
bool Engine::DeleteTransfer(uint64_t id, bool removeFile, std::wstring &error) {
    std::lock_guard lock(mutex_);
    auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](auto &j) { return j->view.id == id; });
    if (found == jobs_.end()) {
        error = L"记录已不存在";
        return false;
    }
    auto job = *found;
    auto active = [](auto &j) {
        return j->view.state == State::Queued || j->view.state == State::Waiting ||
               j->view.state == State::Running;
    };
    if (active(job)) {
        error = L"请先取消传输，再删除记录";
        return false;
    }
    const auto path = job->view.savedPath;
    if (removeFile && !path.empty()) {
        if (std::any_of(jobs_.begin(), jobs_.end(),
                        [&](auto &j) { return active(j) && j->view.savedPath == path; })) {
            error = L"文件仍有未结束的传输";
            return false;
        }
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!job->hasIdentity || !SafeWebPath(path, path.parent_path())) {
                error = L"文件路径已变更，未删除文件";
                return false;
            }
            WinFile file(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            const auto &old = job->fileIdentity;
            if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info) ||
                !WebHandleMatches(file.value, path) ||
                info.dwVolumeSerialNumber != old.dwVolumeSerialNumber ||
                info.nFileIndexHigh != old.nFileIndexHigh || info.nFileIndexLow != old.nFileIndexLow ||
                info.nFileSizeHigh != old.nFileSizeHigh || info.nFileSizeLow != old.nFileSizeLow ||
                CompareFileTime(&info.ftLastWriteTime, &old.ftLastWriteTime) != 0) {
                error = L"文件已修改、替换或被占用，未删除文件";
                return false;
            }
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition,
                                            sizeof(disposition))) {
                error = L"文件删除失败，记录已保留";
                return false;
            }
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
            error = L"无法访问文件，记录已保留";
            return false;
        }
        std::erase_if(webFiles_, [&](auto &entry) { return entry.second.path == path; });
    }
    webFiles_.erase(id);
    if (job->view.folderId.empty()) jobs_.erase(found);
    else { job->view.removed = true; job->view.savedPath.clear(); }
    return true;
}
bool Engine::WaitForIdle(std::chrono::seconds timeout) {
    auto end = Now() + static_cast<uint64_t>(timeout.count()) * 1000;
    while (Now() < end) {
        if (!HasActiveTransfers())
            return true;
        Sleep(50);
    }
    return false;
}
bool Engine::AddPeer(const std::string &endpoint, std::wstring &error) {
    std::string ip;
    unsigned short port;
    if (!Endpoint(endpoint, ip, port) || !IsLocal(ip)) {
        error = L"请输入同一局域网内的 IPv4 地址及端口";
        return false;
    }
    SOCKET s = Connect(ip, port);
    if (s == INVALID_SOCKET) {
        error = L"无法连接设备，检查地址、端口及防火墙";
        return false;
    }
    TrackSocket(s);
    SetTimeout(s, 4000, 4000);
    Config local;
    {
        std::lock_guard lock(mutex_);
        local = config_;
    }
    std::string request = "GET /api/info HTTP/1.1\r\nHost: " + ip +
                          "\r\nConnection: close\r\nX-Chuan-Id: " + Hex(Utf8(local.id)) +
                          "\r\nX-Chuan-Sender: " + Hex(Utf8(local.name)) +
                          "\r\nX-Chuan-Port: " + std::to_string(boundPort_) + "\r\n\r\n",
                first;
    std::map<std::string, std::string> h;
    std::string body;
    uint64_t length = 0;
    bool ok = SendAll(s, request) && ReadHeader(s, first, h) && first.starts_with("HTTP/1.1 200 ") &&
              Number(Get(h, "content-length"), length) && length < 2048;
    if (ok) {
        body.resize(static_cast<size_t>(length));
        ok = ReadExact(s, body.data(), body.size());
    }
    ReleaseSocket(s);
    auto field = [&](const std::string &key) {
        auto pos = body.find("\"" + key + "\":\"");
        if (pos == std::string::npos)
            return std::string{};
        pos += key.size() + 4;
        auto end = body.find('"', pos);
        return body.substr(pos, end - pos);
    };
    auto name = Wide(Unhex(field("nameHex"))), id = Wide(field("id"));
    if (!ok || name.size() > 64 || id.empty() ||
        body.find("\"protocol\":1") == std::string::npos) {
        error = L"目标地址不是兼容的传输软件";
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (id == config_.id) {
            error = L"不能将本机添加为目标设备";
            return false;
        }
        auto it = std::find_if(peers_.begin(), peers_.end(), [&](auto &p) { return p.id == id; });
        Peer peer{id, name.empty() ? AutoNameLocked(Utf8(id)) : name, ip, port, Now(), true};
        if (it == peers_.end()) {
            if (peers_.size() >= 128) {
                error = L"设备数量已达上限";
                return false;
            }
            peers_.push_back(peer);
        } else
            *it = peer;
    }
    return true;
}
#include "presence.inc"
#include "batch.inc"
} // namespace chuan
