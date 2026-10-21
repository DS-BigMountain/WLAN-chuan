#include "core.h"
#include "diagnostics.h"
#include "transfer_view.h"
#include "zip_store.h"
#include "pipeline.h"
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
#include <winioctl.h>

namespace chuan {
namespace {
bool PermanentTransferFailure(const std::wstring &message){
    return message.find(L"同名文件")!=std::wstring::npos||message.find(L"类型冲突")!=std::wstring::npos||
        message.find(L"原文件已保留")!=std::wstring::npos||message.find(L"源文件")!=std::wstring::npos||message.find(L"文件路径已变更")!=std::wstring::npos;
}
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
struct ProcessSecurity {
    bool restrictedToken = false, appContainer = false;
    DWORD integrityLevel = 0;
    bool Restricted() const {
        return restrictedToken || appContainer ||
               (integrityLevel != 0 && integrityLevel < SECURITY_MANDATORY_MEDIUM_RID);
    }
};
ProcessSecurity CurrentProcessSecurity() {
    ProcessSecurity security;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return security;
    DWORD container = 0, bytes = 0;
    GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &bytes);
    security.restrictedToken = IsTokenRestricted(token);
    security.appContainer = container != 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    if (bytes != 0 && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), bytes, &bytes)) {
        const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer.data())->Label.Sid;
        const auto count = *GetSidSubAuthorityCount(sid);
        if (count != 0)
            security.integrityLevel = *GetSidSubAuthority(sid, count - 1);
    }
    CloseHandle(token);
    return security;
}
bool RestrictedProcess() {
    return CurrentProcessSecurity().Restricted();
}
fs::path StorageRelativePath(const fs::path &path, const fs::path &directory) {
    if (!path.is_absolute() || !directory.is_absolute())
        return {};
    const auto relative = path.lexically_normal().lexically_relative(directory.lexically_normal());
    if (relative.empty() || relative.has_root_path())
        return {};
    for (const auto &part : relative)
        if (part == L"..")
            return {};
    return relative;
}
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
    std::string Snapshot(){BCRYPT_HASH_HANDLE copy=nullptr;std::vector<unsigned char> state(object.size());std::array<unsigned char,32> out{};if(BCryptDuplicateHash(hash,&copy,state.data(),static_cast<ULONG>(state.size()),0)<0)throw std::runtime_error("SHA256");const auto status=BCryptFinishHash(copy,out.data(),32,0);BCryptDestroyHash(copy);if(status<0)throw std::runtime_error("SHA256");return Hex(std::string(reinterpret_cast<char*>(out.data()),32));}
};
std::string ResumeKey(HANDLE,uint64_t size){Hash hash;const auto label="chuan-resume-v1:"+std::to_string(size);hash.Add(label.data(),label.size());return hash.Snapshot();}

bool ResumeKeyValid(const std::string &key){return key.size()==64&&std::all_of(key.begin(),key.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
fs::path ResumePath(const fs::path &base,const std::string &owner,const std::wstring &name,uint64_t size,const std::string &key){Hash hash;const auto material=owner+":"+Utf8(name)+":"+std::to_string(size)+":"+key;hash.Add(material.data(),material.size());return base/(L".chuan-resume-"+Wide(hash.Snapshot())+L".part");}
std::wstring Guid() {
    GUID id{};
    CoCreateGuid(&id);
    wchar_t value[40]{};
    StringFromGUID2(id, value, 40);
    return value;
}
bool UserConfigWritable(const fs::path &directory) {
    if (directory.empty())
        return false;
    std::error_code ec;
    const auto ini = directory / L"settings.ini";
    if (fs::exists(ini, ec)) {
        WinFile file(CreateFileW(ini.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
            return false;
    } else if (ec)
        return false;
    auto existing = directory;
    while (!fs::is_directory(existing, ec)) {
        if (ec || fs::exists(existing, ec) || ec)
            return false;
        const auto parent = existing.parent_path();
        if (parent.empty() || parent == existing)
            return false;
        existing = parent;
    }
    const auto probe = existing / (L".chuan-config-probe-" + Guid());
    WinFile file(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    return file.value != INVALID_HANDLE_VALUE;
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
    std::array<char, 4096> buffer{};
    while (all.size() < 16384) {
        // 预读仅用于定位首部结束位置，正文保留在套接字中供流式接收。
        const int count = recv(s, buffer.data(), static_cast<int>(std::min(buffer.size(), 16384 - all.size())), MSG_PEEK);
        if (count <= 0)
            return false;
        const size_t previous = all.size();
        all.append(buffer.data(), static_cast<size_t>(count));
        const auto end = all.find("\r\n\r\n", previous > 3 ? previous - 3 : 0);
        if (end != std::string::npos)
            all.resize(end + 4);
        if (!ReadExact(s, all.data() + previous, all.size() - previous))
            return false;
        if (end != std::string::npos)
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
fs::path AndroidPackagePath() {
    return ExePath().parent_path() / (L"Chuan-" + std::wstring(Version) + L"-android.apk");
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
        const auto portableIni = c.fallbackDataDir / L"settings.ini";
        if (fs::is_regular_file(portableIni, ec) &&
            (GetPrivateProfileIntW(L"Chuan", L"AutomaticStorageFallback", 0, portableIni.c_str()) == 0 ||
             RestrictedProcess() || !UserConfigWritable(c.dataDir)))
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
    const fs::path savedReceiveDir = read(L"ReceiveDir", c.receiveDir.wstring());
    const fs::path savedDataDir = read(L"SavedDataDir", L"");
    c.receiveDir = savedReceiveDir;
    if (!savedReceiveDir.empty() && !savedReceiveDir.has_root_path())
        c.receiveDir = (c.dataDir / savedReceiveDir).lexically_normal();
    else if (const auto relative = StorageRelativePath(savedReceiveDir, savedDataDir); !relative.empty())
        c.receiveDir = (c.dataDir / relative).lexically_normal();
    else if (savedDataDir.empty() && c.dataDir.filename() == L"data" &&
             savedReceiveDir.filename() == L"received" && savedReceiveDir.parent_path().filename() == L"data")
        c.receiveDir = c.dataDir / L"received";
    c.automaticStorageFallback = read(L"AutomaticStorageFallback", L"0") == L"1";
    unsigned short p;
    if (ValidPort(read(L"Port", std::to_wstring(c.port)), p))
        c.port = p;
    c.zipFolders = read(L"ZipFolders", L"0") == L"1";
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
    write(L"SavedDataDir", c.dataDir.wstring());
    write(L"AutomaticStorageFallback", c.automaticStorageFallback ? L"1" : L"0");
    write(L"Port", std::to_wstring(c.port));
    write(L"ZipFolders", c.zipFolders ? L"1" : L"0");
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
        if (error->code().value() == ERROR_ACCESS_DENIED) {
            const auto security = CurrentProcessSecurity();
            if (security.integrityLevel != 0 && security.integrityLevel < SECURITY_MANDATORY_MEDIUM_RID)
                result += L"\n当前进程以低完整性级别运行，Windows 限制其向普通用户目录写入。"
                          L"\n请从文件资源管理器启动发行程序，或指定该启动环境允许写入的目录。";
            else if (security.Restricted())
                result += L"\n检测到受限访问令牌，启动环境可能限制目录写入。"
                          L"\n可从文件资源管理器直接启动程序，或指定该环境允许写入的目录。";
        }
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

Engine::Engine(Config c) : config_(std::move(c)), diagnostic_(std::make_unique<diagnostic::Log>()), webToken_(Hex(Utf8(Guid()))) {}
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
                config_.automaticStorageFallback = true;
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
        const auto originalIni = config_.dataDir / L"settings.ini";
        bool saved = false;
        if (!config_.fallbackDataDir.empty() && config_.fallbackDataDir != config_.dataDir) {
            auto fallback = config_;
            fallback.dataDir = config_.fallbackDataDir;
            fallback.automaticStorageFallback = true;
            try {
                SaveConfig(fallback);
                config_ = std::move(fallback);
                if (!warning.empty()) warning += L"\n\n";
                warning += StorageError(L"原配置目录不可写。", originalIni, exception);
                warning += L"\n已启用便携配置：" + (config_.dataDir / L"settings.ini").wstring();
                saved = true;
            } catch (const std::exception &) {
            }
        }
        if (!saved) {
            if (!warning.empty()) warning += L"\n\n";
            warning += StorageError(L"设置未能保存，本次会话仍可运行。", originalIni, exception) +
                       L"\n请检查该文件是否只读、被占用或缺少写入权限；本次设置可能无法在重启后保留。";
        }
    }
    if (!diagnostic_->Open(config_.dataDir / L"logs")) {
        if (!warning.empty()) warning += L"\n";
        warning += L"传输诊断日志无法写入：" + (config_.dataDir / L"logs").wstring();
    }
    diagnostic_->Write("\"event\":\"session_start\",\"version\":" + Json(Utf8(Version)) + ",\"diagnostic_schema\":2,\"build\":\"" __DATE__ " " __TIME__ "\",\"port\":" + std::to_string(boundPort_));
    const auto processSecurity = CurrentProcessSecurity();
    diagnostic_->Write("\"event\":\"storage\",\"restricted_access_token\":" +
        std::string(processSecurity.restrictedToken ? "true" : "false") +
        ",\"integrity_level\":" + std::to_string(processSecurity.integrityLevel) +
        ",\"app_container\":" + (processSecurity.appContainer ? "true" : "false") +
        ",\"automatic_storage_fallback\":" + (config_.automaticStorageFallback ? "true" : "false") +
        ",\"config_dir\":" + Json(Utf8(config_.dataDir.wstring())) +
        ",\"receive_dir\":" + Json(Utf8(config_.receiveDir.wstring())));
    if (result == NO_ERROR) for (auto a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char address[64]{};
            InetNtopA(AF_INET, &reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr, address, sizeof(address));
            diagnostic_->Write("\"event\":\"interface\",\"name\":" + Json(Utf8(a->FriendlyName ? a->FriendlyName : L"")) +
                ",\"ip\":" + Json(address) + ",\"send_link_bps\":" + std::to_string(a->TransmitLinkSpeed) +
                ",\"receive_link_bps\":" + std::to_string(a->ReceiveLinkSpeed));
        }
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
    running_ = false;retryCv_.notify_all();
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
    if(retryThread_.joinable())retryThread_.join();
    if(retryTimerThread_.joinable())retryTimerThread_.join();
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
    {std::lock_guard lock(mutex_);webArchiveStreams_.clear();}
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
    if (!running_ || (job&&job->retryExpired))
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
    if (!running_ || workers_.size() >= 64)
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
        if (!j->hidden) { out.transfers.push_back(j->view);if(j->retryDeadline&&!j->retryExpired&&!j->cancel&&j->view.state!=State::Completed){out.transfers.back().state=State::Waiting;out.transfers.back().activity=L"独立通道自动重试中";out.transfers.back().error.clear();} if (!j->recipient.empty()) out.transfers.back().awaitingApproval = false;if(j->paused){auto &v=out.transfers.back();v.paused=true;v.state=State::Waiting;v.speed=0;v.error.clear();v.activity=j->remotePaused?L"对方已暂停，已接收数据已保留":L"已暂停，已接收数据已保留";} }
    // Keep the accepted request visible until its concrete manifest arrives.
    // Both sides of this handoff are sampled under the same engine lock.
    for(const auto &job:jobs_)if(job->view.requestOnly&&job->view.state==State::Completed&&!job->cancel&&job->selection.find('1')!=std::string::npos){
        bool registered=false;for(const auto &other:jobs_)if(!other->view.requestOnly&&other->batchId==job->batchId&&other->view.receiving&&other->webClient==job->webClient&&other->nativeOwner==job->nativeOwner){registered=true;break;}
        if(registered||(!job->targetId.empty()&&job->targetId!=config_.id&&job->recipient.empty()))continue;
        auto pending=job->view;pending.requestOnly=false;pending.awaitingApproval=false;pending.state=State::Waiting;pending.activity=L"已接收，等待发送端开始";pending.error.clear();pending.total=0;pending.done=0;
        size_t accepted=0;for(size_t i=0;i<job->view.offerItems.size();++i)if(i<job->selection.size()&&job->selection[i]=='1'){if(!accepted)pending.name=job->view.offerItems[i].name;pending.total+=job->view.offerItems[i].total;++accepted;}if(accepted>1)pending.name+=L" 等 "+std::to_wstring(accepted)+L" 项";out.transfers.push_back(std::move(pending));
    }
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
            peer.platform = it->platform;
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
    if (!Get(headers, "x-chuan-server").empty()) {
        std::lock_guard lock(mutex_);
        if (Get(headers, "x-chuan-server") != Utf8(config_.id)) {
            Reply(s, 409, "Conflict", Utf8(L"服务端身份已变化，请重新连接")); return;
        }
    }
    if (path.starts_with("/api/"))
        ObserveNative(headers, ip);
    if ((path == "/" || path.starts_with("/web/")) && HandleWeb(s, ip, method, path, headers))
        return;
    if (method == "POST" && path == "/api/offer") { ReceiveOffer(s, headers, ip, false); return; }
    if(method=="POST"&&path.starts_with("/api/file-phase/")){ReceivePhase(s,headers,ip,path.substr(16),false);return;}
    if (method == "POST" && (path == "/api/offer-status" || path == "/api/offer-cancel")) { ReceiveOfferStatus(s, headers, ip, false, path.ends_with("cancel")); return; }
    if(method=="POST"&&path.starts_with("/api/retry-upload/")){RetryUpload(s,headers,ip,path.substr(18),false);return;}
    if(method=="POST"&&(path=="/api/batch-files"||path=="/api/batch-status")){ReceiveBatchFiles(s,headers,ip,path.ends_with("batch-status"),false);return;}
    if(method=="POST"&&path.starts_with("/api/upload-status/")){
        const auto upload=path.substr(19);std::lock_guard lock(mutex_);
        for(const auto &job:jobs_)if(job->webUpload==upload&&job->nativeOwner==Get(headers,"x-chuan-id")&&job->batchId==Get(headers,"x-chuan-batch")){
            const auto batch=batchApprovals_.find("native:"+ip+":"+job->nativeOwner+":"+job->batchId);if(batch!=batchApprovals_.end()){batch->second.touched=Now();Reply(s,200,"OK",std::to_string(job->paused?7:static_cast<int>(job->view.state)));return;}}
        Reply(s,404,"Not Found");return;
    }
    if (method == "POST" && path == "/api/copy-check") { auto check=headers;check["x-chuan-metadata"]="1";ReceiveUpload(s,check,ip,false,true);return; }
    if (method == "POST" && path == "/api/copy-probe") { ReceiveUpload(s, headers, ip, false, true); return; }
    if(method=="POST"&&path.starts_with("/api/pause-upload/")){uint64_t id=0;{std::lock_guard lock(mutex_);for(const auto &job:jobs_)if(job->webUpload==path.substr(18)&&job->nativeOwner==Get(headers,"x-chuan-id")&&job->batchId==Get(headers,"x-chuan-batch")){id=job->view.id;break;}}if(id){Pause(id,Get(headers,"x-chuan-paused")!="0");Reply(s,200,"OK");}else Reply(s,404,"Not Found");return;}
    if(method=="POST"&&path=="/api/resume"){ResumeProbe(s,headers,ip,false);return;}
    if (method == "POST" && path == "/api/upload") {
        ReceiveUpload(s, headers, ip);
        return;
    }
    if (method == "POST" && path == "/api/leave") {
        auto id = Wide(Unhex(Get(headers, "x-chuan-id")));
        std::lock_guard lock(mutex_);
        for (const auto &job:jobs_) if (job->nativeOwner==Get(headers,"x-chuan-id") && !job->asyncOfferKey.empty() && job->view.state<State::Completed) {
            job->cancel=true;CompleteOfferLocked(job,std::string(job->view.offerItems.size(),'0'));job->view.error=L"发送端已关闭，接收请求已取消";
        }
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
                  ",\"port\":" + std::to_string(boundPort_) + ",\"instance\":" + Json(webToken_) + ",\"platform\":\"windows\"}",
              "application/json; charset=utf-8");
        return;
    }
    if (!cfg.shareSoftware) {
        Reply(s, 403, "Forbidden", Utf8(L"Web服务已关闭"));
        return;
    }
    const bool androidPackage = path == "/download/Chuan.apk";
    if (path != "/download/Chuan.exe" && !androidPackage) {
        Reply(s, 404, "Not Found");
        return;
    }
    const auto downloadPath = androidPackage ? AndroidPackagePath() : ExePath();
    WinFile file(CreateFileW(downloadPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER size{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size)) {
        Reply(s, androidPackage ? 404 : 500, androidPackage ? "Not Found" : "Internal Server Error");
        return;
    }
    if (!SendAll(s, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: " +
                    std::string(androidPackage ? "application/vnd.android.package-archive" : "application/octet-stream") +
                    "\r\nContent-Disposition: attachment; filename=\"" +
                    (androidPackage ? "Chuan-" + Utf8(Version) + "-android.apk" : "Chuan.exe") +
                    "\"\r\nX-Content-Type-Options: nosniff\r\nCache-Control: "
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
    auto platform=Get(h,"x-chuan-platform");peer.platform=platform=="windows"||platform=="android"?platform:found!=peers_.end()?found->platform:"";
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
    if (!Get(h, "x-chuan-instance").empty() && Get(h, "x-chuan-instance") != webToken_) {
        Reply(s, 409, "Conflict", Utf8(L"服务端运行会话已变化，请刷新页面")); return true;
    }
    if (method == "POST") {
        bool validSession = false;
        { std::lock_guard lock(mutex_);
          auto it = browsers_.find(Get(h, "x-chuan-client"));
          validSession = it != browsers_.end() && it->second.token == Get(h, "x-chuan-token");
          if(validSession&&path!="/web/api/leave"){auto &session=it->second;const auto tab=Get(h,"x-chuan-tab");session.tabs[tab]=Now();session.openTabs.insert(tab);for(auto &peer:peers_)if(peer.id==session.id)peer.seen=Now();} }
        if (!validSession) {
            Reply(s, 403, "Forbidden", Utf8(L"请求无效，请刷新网页"));
            return true;
        }
        if(path=="/web/api/diagnostics-export"){
            try{const auto file=ExportDiagnostics(Get(h,"x-chuan-client"));std::ifstream input(file,std::ios::binary);
                const auto length=fs::file_size(file);SendAll(s,"HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: application/x-ndjson\r\nContent-Disposition: attachment; filename=Chuan-diagnostics.jsonl\r\nContent-Length: "+std::to_string(length)+"\r\n\r\n");
                std::array<char,65536> buffer;while(input){input.read(buffer.data(),buffer.size());if(input.gcount()&&!SendAll(s,buffer.data(),static_cast<size_t>(input.gcount())))break;}
                input.close();std::error_code ec;fs::remove(file,ec);
            }catch(...){Reply(s,500,"Internal Server Error");}return true;
        }
        if (path == "/web/api/diagnostic") {
            // One bounded, numeric report per upload, restricted to its owning session.
            if ((!Get(h, "content-length").empty() && Get(h, "content-length") != "0") || !Get(h, "transfer-encoding").empty()) {
                Reply(s, 400, "Bad Request"); return true;
            }
            uint64_t id = 0; std::string fields;
            for (const auto *key : {"hash-ms", "upload-ms", "body-ms", "status"}) {
                uint64_t value = 0;
                if (!Number(Get(h, std::string("x-chuan-") + key), value) || value > 604800000) {
                    Reply(s, 400, "Bad Request"); return true;
                }
                fields += "," + Json(key) + ":" + std::to_string(value);
            }
            { std::lock_guard lock(mutex_);
              for (auto &job : jobs_) if (job->webClient == Get(h, "x-chuan-client") &&
                  job->webUpload == Get(h, "x-chuan-upload") && job->view.receiving && !job->diagnosticReported) {
                  job->diagnosticReported = true; id = job->view.id; break;
              } }
            if (!id) { Reply(s, 404, "Not Found"); return true; }
            diagnostic_->Write("\"event\":\"browser_upload_report\",\"id\":" + std::to_string(id) + fields);
            Reply(s, 200, "OK"); return true;
        }
        if (path == "/web/api/offer") { ReceiveOffer(s, h, ip, true); return true; }
        if (path == "/web/api/offer-cancel") { ReceiveOfferStatus(s, h, ip, true, true); return true; }
        if(path.starts_with("/web/api/retry-upload/")){RetryUpload(s,h,ip,path.substr(22),true);return true;}
        if (path == "/web/api/batch-files" || path == "/web/api/batch-status") { ReceiveBatchFiles(s, h, ip, path.ends_with("batch-status")); return true; }
        if(path.starts_with("/web/api/file-phase/")){ReceivePhase(s,h,ip,path.substr(20),true);return true;}
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
            if (!ReadExact(s, selection.data(), selection.size()) || selection.find_first_not_of("01-") != std::string::npos) {
                Reply(s, 400, "Bad Request"); return true;
            }
            DecideItems(id, selection); Reply(s, 200, "OK"); return true;
        }
        if (path == "/web/api/copy-check") { auto check=h;check["x-chuan-metadata"]="1";ReceiveUpload(s,check,ip,true,true);return true; }
        if (path == "/web/api/copy-probe") { ReceiveUpload(s, h, ip, true, true); return true; }
        if(path.starts_with("/web/api/archive/")){WebArchive(s,h,ip,path);return true;}
        if(path.starts_with("/web/api/pause/")||path.starts_with("/web/api/resume-job/")){uint64_t id=0;const bool pause=path.starts_with("/web/api/pause/");const auto value=path.substr(pause?15:20);{std::lock_guard lock(mutex_);for(const auto &job:jobs_)if((std::to_string(job->view.id)==value||job->webUpload==value)&&(job->webClient==Get(h,"x-chuan-client")||job->recipient==Get(h,"x-chuan-client"))){id=job->view.id;break;}}if(!id){Reply(s,404,"Not Found");return true;}Pause(id,pause);Reply(s,200,"OK");return true;}
        if(path=="/web/api/resume"){ResumeProbe(s,h,ip,true);return true;}
        if (path == "/web/api/upload") {
            ReceiveUpload(s, h, ip, true);
            return true;
        }
        if (Get(h, "content-length") != "0" || !Get(h, "transfer-encoding").empty()) {
            Reply(s, 400, "Bad Request");
            return true;
        }
        if (path.starts_with("/web/api/file-info/") || path.starts_with("/web/api/file-saved/") || path.starts_with("/web/api/file-failed/")) {
            uint64_t id=0;WebFile file;
            if(!Number(path.substr(path.find_last_of('/')+1),id)){Reply(s,400,"Bad Request");return true;}
            {std::lock_guard lock(mutex_);auto it=webFiles_.find(id);if(it==webFiles_.end()||it->second.recipient!=Get(h,"x-chuan-client")||!it->second.delivery){Reply(s,404,"Not Found");return true;}file=it->second;}
            const auto job=file.delivery;
            if(job->cancel){Reply(s,410,"Gone");return true;}
            if(path.starts_with("/web/api/file-failed/")){SetState(job,State::Failed,L"浏览器保存失败或存在内容冲突，原文件已保留");Reply(s,200,"OK");return true;}
            if(path.starts_with("/web/api/file-saved/")){Progress(job,file.size);SetState(job,State::Completed);Reply(s,200,"OK");return true;}
            try {
                std::string digest;
                if(!job->view.directory){
                    if(!SafeWebPath(file.path,file.root))throw std::runtime_error("path");
                    WinFile input(CreateFileW(file.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                    if(input.value==INVALID_HANDLE_VALUE||!WebHandleMatches(input.value,file.path))throw std::runtime_error("file");
                    Hash hash;std::vector<char> bytes(256*1024);DWORD count;uint64_t tick=Now();
                    for(;;){if(job->cancel||!ReadFile(input.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr))throw std::runtime_error("read");if(!count)break;hash.Add(bytes.data(),count);if(Now()-tick>2000){if(!SendAll(s,"HTTP/1.1 102 Processing\r\n\r\n"))throw std::runtime_error("connection");tick=Now();}}
                    auto value=hash.Finish();digest=Hex(std::string(reinterpret_cast<char*>(value.data()),value.size()));
                }
                Reply(s,200,"OK","{\"directory\":"+std::string(job->view.directory?"true":"false")+",\"sha256\":"+Json(digest)+"}","application/json");
            }catch(...){Reply(s,409,"Conflict",Utf8(L"无法核对发送文件"));}return true;
        }
        if (path.starts_with("/web/api/upload-status/")) {
            const auto key=path.substr(23),owner=Get(h,"x-chuan-client");
            std::lock_guard lock(mutex_);
            const auto found=std::find_if(jobs_.begin(),jobs_.end(),[&](const auto &job){return job->webClient==owner && job->webUpload==key;});
            if (found==jobs_.end()) { Reply(s,404,"Not Found");return true; }
            const auto &job=**found;
            Reply(s,200,"OK","{\"id\":"+Json(std::to_string(job.view.id))+",\"state\":"+std::to_string(static_cast<int>(job.view.state))+",\"paused\":"+(job.paused?"true":"false")+",\"done\":"+std::to_string(job.view.done)+",\"error\":"+Json(Utf8(job.view.error))+"}","application/json; charset=utf-8");return true;
        }
        if (path.starts_with("/web/api/wait/")) {
            uint64_t id = 0;
            if (!Number(path.substr(14), id)) { Reply(s, 400, "Bad Request"); return true; }
            std::string body;
            {
                std::unique_lock lock(mutex_);
                const auto client = Get(h, "x-chuan-client");
                auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](auto &job) {
                    return job->view.id == id && job->webClient == client && !job->view.receiving;
                });
                if (found == jobs_.end()) { lock.unlock(); Reply(s, 404, "Not Found"); return true; }
                const auto job = *found;
                // 等待释放引擎锁；每次最多占用工作线程一秒，完成或取消时立即唤醒。
                job->cv.wait_for(lock, std::chrono::seconds(1), [&] {
                    return !running_ || job->cancel || job->view.state >= State::Completed;
                });
                const auto state = job->cancel ? State::Cancelled : job->view.state;
                body = "{\"state\":" + std::to_string(static_cast<int>(state)) +
                       ",\"error\":" + Json(Utf8(job->view.error)) + "}";
            }
            Reply(s, 200, "OK", body, "application/json; charset=utf-8");
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
            session.openTabs.erase(Get(h, "x-chuan-tab"));
            if (session.tabs.empty()) std::erase_if(peers_, [&](auto &p) { return p.id == session.id; });
            ExpireLocked();
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
              for (auto &p : peers_) if (p.id == id && PeerSeenRecently(p,Now())) { peer = p; found = true; }
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
            std::set<std::string> active;for(const auto &job:jobs_)if(job->paused||job->view.state<State::Completed||job->retryDeadline)active.insert(job->batchId);
            for (auto &j : jobs_) if (j->asyncOfferKey.empty()&&!active.contains(j->batchId)&&j->view.state >= State::Completed && !busy.contains(TransferGroup(j->view)) && (j->webClient == client || j->recipient == client))
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
                    Reply(s, 202, "Accepted", "{\"id\":" + Json(std::to_string(existing->view.id)) + "}", "application/json; charset=utf-8"); return true;
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
            if ((!file.delivery || !file.delivery->view.directory) && (!SafeWebPath(file.path, file.root) || !unchanged)) {
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
                    job->view.directory = upload->view.directory;
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
                Reply(s, 202, "Accepted", "{\"id\":" + Json(std::to_string(job->view.id)) + "}", "application/json; charset=utf-8");
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
                : path == "/web/session.js" ? 207
                : path == "/web/zip_store.js" ? 208
                : path == "/web/zip_worker.js" ? 209
                : path == "/web/approvals.js" ? 210
                : path == "/web/hash_file.js" ? 211
                : path == "/web/hash_worker.js" ? 212
                : path == "/web/zip_stream_worker.js" ? 213
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
            const auto device=Get(h,"x-chuan-device");auto agent=Get(h,"user-agent");std::transform(agent.begin(),agent.end(),agent.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});
            if(device=="mobile-web"||device=="desktop-web")current.platform=device;
            else if(!agent.empty())current.platform=agent.find("android")!=std::string::npos||agent.find("iphone")!=std::string::npos||agent.find("ipad")!=std::string::npos||agent.find("mobile")!=std::string::npos?"mobile-web":"desktop-web";
            else if(current.platform.empty())current.platform="web";
            current.tabs[Get(h, "x-chuan-tab")] = Now();
            current.openTabs.insert(Get(h, "x-chuan-tab"));
            uint64_t ack = 0;
            if (Number(Get(h, "x-chuan-ping-ack"), ack) && ack <= current.ping) current.ack = std::max(current.ack, ack);
            Peer peer{current.id, name, ip, boundPort_, Now(), false, true};
            current.directoryReceive=Get(h,"x-chuan-directory-receive")=="1";peer.directoryReceive=current.directoryReceive;
            peer.platform=current.platform;
            auto found = std::find_if(peers_.begin(), peers_.end(), [&](auto &p) { return p.id == peer.id; });
            if (found != peers_.end()) *found = peer;
            else { peers_.push_back(peer); refresh_ = true; }
            session = current;
        }
        if (newSession && !coordinator.id.empty()) SyncMesh(coordinator);
        auto snapshot = GetSnapshot();
        const auto clientIp = ip.starts_with("127.") && !snapshot.addresses.empty() ? snapshot.addresses.front() : ip;
        std::error_code apkError;
        const bool androidAvailable = fs::is_regular_file(AndroidPackagePath(), apkError);
        std::ostringstream out;
        out << "{\"name\":" << Json(Utf8(snapshot.config.name)) << ",\"version\":" << Json(Utf8(Version))
            << ",\"serverInstance\":" << Json(webToken_) << ",\"platform\":\"windows\""
            << ",\"androidAvailable\":" << (androidAvailable ? "true" : "false")
            << ",\"webUploadChecksumOptional\":true,\"webCopyCheck\":true,\"webDownloadChecksum\":true,\"webBatchFiles\":true,\"webUploadStatus\":true,\"webZipArchives\":true,\"webZipStream\":true,\"webResume\":true,\"webOfferAsync\":true"
            << ",\"webTransferWait\":true"
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
                << ",\"note\":" << Json(Utf8(session.notes[p.ip])) << ",\"ip\":" << Json(p.ip) << ",\"browser\":" << (p.browser ? "true" : "false") << ",\"platform\":" << Json(p.platform) << ",\"directoryReceive\":" << (p.directoryReceive?"true":"false") << '}';
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
        out << "],\"offers\":[";
        comma = false;
        { std::lock_guard lock(mutex_);
          for (const auto &j : jobs_) if (j->webClient == client && !j->asyncOfferKey.empty()) {
              if (comma) out << ','; comma = true;
              out << "{\"id\":" << Json(std::to_string(j->view.id)) << ",\"batch\":" << Json(j->batchId)
                  << ",\"state\":" << static_cast<int>(j->view.state) << ",\"selection\":" << Json(j->selection)
                  << ",\"error\":" << Json(Utf8(j->view.error)) << '}';
          }
        }
        out << "],\"requests\":[";
        comma = false;
        { std::lock_guard lock(mutex_);
          for (auto &j : jobs_) if (j->recipient == client && j->view.requestOnly && j->view.awaitingApproval) {
              if (comma) out << ','; comma = true;
              out << "{\"id\":" << Json(std::to_string(j->view.id)) << ",\"sender\":" << Json(Utf8(j->view.peer)) << ",\"batch\":" << Json(j->batchId) << ",\"selection\":" << Json(j->selection) << ",\"items\":[";
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
            std::string webStage;
            std::wstring webTarget;
            {
                std::lock_guard lock(mutex_);
                auto found = stateJobs.find(j.id);
                if(found!=stateJobs.end()&&found->second->view.requestOnly&&found->second->recipient!=client)continue;
                if (found != stateJobs.end()) {
                    auto &job = *found->second;
                    if (job.hiddenClients.contains(client)) { if (j.folderId.empty()) continue; removed = true; }
                    canDelete = !client.empty() && job.webClient == client;
                    if (canDelete) webTarget = job.targetId.empty() ? snapshot.config.id : job.targetId;
                    if (job.webClient == client && !job.webUpload.empty())
                        webStage = job.view.receiving ?
                            (job.targetId.empty() || job.targetId == snapshot.config.id ? "upload" : "staging") : "forward";
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
                << ",\"folderStopped\":" << (j.folderStopped?"true":"false")
                << ",\"directory\":" << (j.directory?"true":"false") << ",\"skipped\":" << (j.skipped?"true":"false")
                << ",\"folderId\":" << Json(j.folderId) << ",\"folderName\":" << Json(Utf8(j.folderName))
                << ",\"folderCount\":" << j.folderCount << ",\"folderTotal\":" << j.folderTotal
                << ",\"removed\":" << (removed ? "true" : "false")
                << ",\"activity\":" << Json(Utf8(j.state<State::Completed?j.activity:L""))
                << ",\"paused\":" << (j.paused?"true":"false") << ",\"stateText\":" << Json(Utf8(j.skipped?L"内容相同，已跳过":j.activity.empty() || j.state>=State::Completed ? StateText(j.state, receiving) : j.activity))
                << ",\"receiving\":" << (receiving ? "true" : "false")
                << ",\"canDelete\":" << (canDelete ? "true" : "false")
                << ",\"stage\":" << Json(webStage)
                << ",\"target\":" << Json(Utf8(webTarget))
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
        std::map<std::string, std::string> parameters;
        if (query != std::string::npos) {
            std::istringstream fields(path.substr(query + 1)); std::string field;
            while (std::getline(fields, field, '&')) {
                auto equal = field.find('='); if (equal == std::string::npos) continue;
                std::string value; const auto encoded = field.substr(equal + 1);
                for (size_t i = 0; i < encoded.size(); ++i) {
                    if (encoded[i] == '%' && i + 2 < encoded.size()) { value += Unhex(encoded.substr(i + 1, 2)); i += 2; }
                    else value += encoded[i] == '+' ? ' ' : encoded[i];
                }
                parameters[field.substr(0, equal)] = value;
            }
            client = parameters["client"];
        }
        if ((!parameters["server"].empty() && parameters["server"] != Utf8(cfg.id)) ||
            (!parameters["instance"].empty() && parameters["instance"] != webToken_)) {
            Reply(s, 409, "Conflict", Utf8(L"下载链接不属于当前服务端运行会话")); return true;
        }
        if (!parameters["server"].empty()) {
            std::lock_guard lock(mutex_); const auto session = browsers_.find(client);
            if (session == browsers_.end() || parameters["ticket"] != session->second.token) {
                Reply(s, 401, "Unauthorized"); return true;
            }
        }
        if (!Number(path.substr(11, query == std::string::npos ? std::string::npos : query - 11), id))
            Reply(s, 404, "Not Found");
        else {
            uint64_t offset=0;auto start=Get(h,"x-chuan-offset");bool range=false;if(start.empty()&&!Get(h,"range").empty()){const auto value=Get(h,"range");if(!value.starts_with("bytes=")||!value.ends_with("-")){Reply(s,416,"Range Not Satisfiable");return true;}start=value.substr(6,value.size()-7);range=true;}if(!start.empty()&&!Number(start,offset)){Reply(s,416,"Range Not Satisfiable");return true;}WebDownload(s, ip, id, client,parameters["checksum"]=="trailer",offset,Get(h,"x-chuan-prefix"),range);
        }
        return true;
    }
    Reply(s, 404, "Not Found");
    return true;
}
void Engine::WebDownload(SOCKET s, const std::string &ip, uint64_t id, const std::string &client,bool checksum,uint64_t offset,const std::string &prefix,bool range) {
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
    if(offset>entry.size||(offset&&!prefix.empty()&&!ResumeKeyValid(prefix))||(range&&(checksum||offset>=entry.size))){Reply(s,416,"Range Not Satisfiable");return;}
    const auto responseStatus=range?"HTTP/1.1 206 Partial Content\r\n":"HTTP/1.1 200 OK\r\n";
    const auto contentRange=range?"Content-Range: bytes "+std::to_string(offset)+"-"+std::to_string(entry.size?entry.size-1:0)+"/"+std::to_string(entry.size)+"\r\n":std::string{};
    if(entry.delivery&&entry.delivery->archivePlan){
        const auto job=entry.delivery;
        {std::lock_guard lock(mutex_);if(job->paused||job->cancel||job->view.state==State::Running||!config_.shareSoftware){Reply(s,409,"Conflict");return;}}
        TrackSocket(s,job);SetState(job,State::Running);{std::lock_guard lock(mutex_);job->view.activity=L"边打包边发送 ZIP";TraceJobLocked(job);}
        try{
            Hash hash;if(offset&&!prefix.empty()){struct PrefixDone{};uint64_t checked=0;try{archive::Emit(*job->archivePlan,[&](const void *data,size_t count){const auto take=static_cast<size_t>(std::min<uint64_t>(count,offset-checked));hash.Add(data,take);checked+=take;if(checked==offset)throw PrefixDone{};},[](uint64_t,const std::wstring &){},[&]{return job->paused||job->cancel||!running_;});}catch(const PrefixDone&){}if(hash.Snapshot()!=prefix){SetState(job,State::Failed,L"续传内容不一致");Reply(s,409,"Conflict","续传内容不一致");return;}}
            if(!SendAll(s,std::string(responseStatus)+contentRange+"Accept-Ranges: bytes\r\nConnection: close\r\nContent-Type: application/zip\r\nCache-Control: no-store\r\nContent-Disposition: attachment; filename=\"download.zip\"; filename*=UTF-8''"+PercentEncode(Utf8(job->view.name))+"\r\nContent-Length: "+std::to_string(entry.size-offset+(checksum?32:0))+"\r\n\r\n"))throw std::runtime_error("send");
            uint64_t done=offset,generated=0;job->progressBase=offset;
            archive::Emit(*job->archivePlan,[&](const void *data,size_t count){const auto skip=static_cast<size_t>(std::min<uint64_t>(count,offset>generated?offset-generated:0));generated+=count;if(checksum&&prefix.empty()&&skip)hash.Add(data,skip);data=static_cast<const char*>(data)+skip;count-=skip;if(!count)return;if(checksum)hash.Add(data,count);if(!SendAll(s,data,count))throw std::runtime_error("send:"+std::to_string(WSAGetLastError()));done+=count;Progress(job,done);},[](uint64_t,const std::wstring &){},[&]{return job->paused||job->cancel||!running_;});
            if(checksum){const auto digest=hash.Finish();if(!SendAll(s,digest.data(),digest.size()))throw std::runtime_error("send");}SetState(job,State::Completed);
        }catch(const std::exception &error){SetState(job,job->cancel?State::Cancelled:State::Failed,Wide(error.what()));}return;
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
        if (job->paused || job->view.state == State::Running || job->cancel) {
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
    {std::lock_guard lock(mutex_);job->view.activity=L"正在发送文件";TraceJobLocked(job);}
    diagnostic::Transfer trace(*diagnostic_, s, job->view.id, entry.size, "web_download", ",\"client_id\":"+Json(job->webClient)+",\"recipient_id\":"+Json(job->recipient)+",\"batch_id\":"+Json(job->batchId)+",\"upload_id\":"+Json(job->webUpload));
    std::unique_ptr<Hash> hash;if(checksum||!prefix.empty())hash=std::make_unique<Hash>();
    if(offset){if(hash){std::vector<char> bytes(1024*1024);uint64_t checked=0;while(checked<offset){DWORD count=0;if(job->paused||job->cancel||!ReadFile(file.value,bytes.data(),static_cast<DWORD>(std::min<uint64_t>(bytes.size(),offset-checked)),&count,nullptr)||!count){SetState(job,State::Failed);Reply(s,409,"Conflict");return;}hash->Add(bytes.data(),count);checked+=count;}if(!prefix.empty()&&hash->Snapshot()!=prefix){SetState(job,State::Failed,L"续传内容不一致");Reply(s,409,"Conflict","续传内容不一致");return;}}else{LARGE_INTEGER position{};position.QuadPart=offset;if(!SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN)){Reply(s,409,"Conflict");return;}}}
    if(!checksum)hash.reset();job->progressBase=offset;trace.StartAt(offset);
    bool ok =
        SendAll(s, std::string(responseStatus)+contentRange+"Accept-Ranges: bytes\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n"
                   "Content-Disposition: attachment; filename=\"download\"; filename*=UTF-8''" +
                       PercentEncode(Utf8(entry.path.filename().wstring())) +
                       "\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\nContent-Length: " +
                       std::to_string(entry.size-offset+(checksum?32:0)) + "\r\n\r\n");
    std::vector<char> buffer(256 * 1024);
    uint64_t done = offset;
    while (ok && running_ && !job->paused && !job->cancel && done < entry.size) {
        DWORD read = 0;
        { diagnostic::Measure time(trace.diskMs);
          ok = ReadFile(file.value, buffer.data(), static_cast<DWORD>(std::min<uint64_t>(buffer.size(),entry.size-done)), &read, nullptr) && read;
          if (!ok) { trace.fileError = GetLastError(); trace.failureStage = "read"; } }
        if(ok&&hash)hash->Add(buffer.data(),read);
        if (ok) { diagnostic::Measure time(trace.networkMs);
            ok = SendAll(s, buffer.data(), read);
            if (!ok) { trace.socketError = WSAGetLastError(); trace.failureStage = "send"; }
        }
        if (ok) {
            done += read;
            Progress(job, done);
            trace.done = done; trace.Sample();
        }
    }
    if(ok&&done==entry.size&&hash&&!job->cancel){const auto digest=hash->Finish();ok=SendAll(s,digest.data(),digest.size());}
    trace.result = job->cancel || !running_ ? "cancelled" : ok && done == entry.size ? "sent_to_socket" : "failed";
    if(ok&&done==entry.size&&running_&&!job->cancel&&entry.delivery&&!job->view.folderId.empty()){
        std::lock_guard lock(mutex_);if(job->view.state<State::Completed){job->view.state=State::Waiting;job->view.activity=L"等待浏览器保存确认";TraceJobLocked(job);}
    } else SetState(job,job->cancel||!running_?State::Cancelled:ok&&done==entry.size?State::Completed:State::Failed);
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
    jobs_.push_back(j);TraceJobLocked(j);
    return j;
}
fs::path Engine::ExportDiagnostics(const std::string &client){
    fs::path directory;{std::lock_guard lock(mutex_);directory=config_.dataDir/L"diagnostic-exports";for(const auto &job:jobs_)TraceJobLocked(job);}
    fs::create_directories(directory);const auto target=directory/(L"Chuan-diagnostics-"+std::to_wstring(Now())+L"-"+Guid()+L".jsonl");
    if(!diagnostic_->Export(target,client.empty()?"":Json(client)))throw std::runtime_error("日志导出失败，请检查可用空间与写入权限");return target;
}
void Engine::TraceJobLocked(const std::shared_ptr<Job> &j,bool sample){
    const auto tick=Now();const bool changed=j->traceState!=static_cast<int>(j->view.state)||j->tracePhase!=j->view.activity||j->traceError!=j->view.error;
    if(!changed&&(!sample||tick-j->traceSample<1000))return;
    if(!j->traceCreated)j->traceCreated=tick;
    const auto elapsed=j->traceSample?tick-j->traceSample:0;
    diagnostic_->Write("\"event\":"+Json(changed?"file_phase":"file_sample")+",\"id\":"+std::to_string(j->view.id)+
      ",\"batch_id\":"+Json(j->batchId)+",\"upload_id\":"+Json(j->webUpload)+",\"folder_id\":"+Json(j->folderId)+
      ",\"client_id\":"+Json(j->webClient)+",\"recipient_id\":"+Json(j->recipient)+",\"path\":"+Json(Utf8(j->view.name))+
      ",\"peer\":"+Json(Utf8(j->view.peer))+",\"receiving\":"+(j->view.receiving?"true":"false")+",\"directory\":"+(j->view.directory?"true":"false")+
      ",\"state\":"+std::to_string(static_cast<int>(j->view.state))+",\"phase\":"+Json(Utf8(j->view.activity))+
      ",\"previous_phase\":"+Json(Utf8(j->tracePhase))+",\"previous_state\":"+std::to_string(j->traceState)+",\"previous_ms\":"+std::to_string(j->traceAt?tick-j->traceAt:0)+
      ",\"elapsed_ms\":"+std::to_string(tick-j->traceCreated)+",\"done_bytes\":"+std::to_string(j->view.done)+",\"total_bytes\":"+std::to_string(j->view.total)+
      ",\"interval_ms\":"+std::to_string(elapsed)+",\"interval_bytes\":"+std::to_string(j->view.done>=j->traceBytes?j->view.done-j->traceBytes:0)+
      ",\"skipped\":"+(j->view.skipped?"true":"false")+",\"retry_pending\":"+(j->retryDeadline?"true":"false")+",\"error\":"+Json(Utf8(j->view.error)));
    if(changed){j->traceAt=tick;j->tracePhase=j->view.activity;j->traceState=static_cast<int>(j->view.state);j->traceError=j->view.error;}
    j->traceSample=tick;j->traceBytes=j->view.done;
}
void Engine::SetState(const std::shared_ptr<Job> &j, State s, const std::wstring &error) {
    std::lock_guard lock(mutex_);
    if(j->cancel&&s<State::Completed)s=State::Cancelled;
    if(j->retryExpired&&s!=State::Completed&&s!=State::Cancelled){s=State::Failed;j->view.activity.clear();}
    if (s==State::Running && j->view.state!=State::Running) j->view.started=Now();
    if(s==State::Completed||s==State::Cancelled){j->paused=false;j->remotePaused=false;j->view.paused=false;}
    j->view.state = s;
    j->view.error = j->retryExpired&&s==State::Failed?L"传输失败超时（自动重试已达 30 秒）":error;
    if (s >= State::Completed) { j->finishedAt = Now(); j->view.awaitingApproval = false; j->view.activity.clear(); }
    TraceJobLocked(j);j->cv.notify_all();
}
void Engine::Progress(const std::shared_ptr<Job> &j, uint64_t done) {
    std::lock_guard lock(mutex_);
    if(j->retryDeadline&&done>j->view.done)j->retryDeadline=Now()+30000;
    j->view.done = done;TraceJobLocked(j,true);
    uint64_t ms = Now() - j->view.started;
    if (ms)
        j->view.speed = static_cast<double>(done>=j->progressBase?done-j->progressBase:0) * 1000 / static_cast<double>(ms);
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
    if(!job->archivePlan)RememberFile(job, path);
    std::lock_guard lock(mutex_);
    auto online = std::find_if(peers_.begin(), peers_.end(), [&](auto &p) {
        return p.browser && p.id == peer.id && PeerSeenRecently(p,Now());
    });
    if (!config_.shareSoftware || online == peers_.end() || job->cancel || (!job->hasIdentity && !job->archivePlan && !job->view.directory)) {
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
    job->view.activity=L"等待浏览器接收";TraceJobLocked(job);
    job->view.done = 0; job->view.speed = 0; job->view.started = 0;
    auto absolute = job->archivePlan?fs::path(job->view.name):job->view.savedPath;
    webFiles_[job->view.id] = {absolute, root.empty() ? absolute.parent_path() : root, job->view.total,
                               job->recipient, job, {}, job->fileIdentity, job->hasIdentity, job->view.name};
}
#include "web_archive.inc"
#include "resume.inc"
void Engine::ReceiveUpload(SOCKET s, const std::map<std::string, std::string> &h, const std::string &ip,
                           bool browser, bool probe) {
    const bool directory = Get(h, "x-chuan-directory") == "1";
    const bool copying = Get(h, "x-chuan-copy") == "1";
    const auto name = SanitizeRelative(Wide(Unhex(Get(h, "x-chuan-path"))));
    auto sender = Wide(Unhex(Get(h, Get(h, "x-chuan-display-sender").empty() ? "x-chuan-sender" : "x-chuan-display-sender")));
    uint64_t total = 0, length = 0;
    const auto checksum = Get(h, "x-chuan-checksum");
    const bool verifyChecksum = !(browser && checksum == "none");
    const uint64_t trailerBytes = verifyChecksum ? 32 : 0;
    const auto resumeKey=Get(h,"x-chuan-resume-key");uint64_t resumeOffset=0;
    if((!resumeKey.empty()&&!ResumeKeyValid(resumeKey))||(!Get(h,"x-chuan-offset").empty()&&!Number(Get(h,"x-chuan-offset"),resumeOffset))){Reply(s,400,"Bad Request");return;}
    if (name.empty() || sender.empty() || sender.size() > 64 ||
        (!checksum.empty() && checksum != "sha256" && !(browser && checksum == "none")) ||
        sender.find_first_of(L"\r\n") != std::wstring::npos || !Number(Get(h, "x-chuan-size"), total) ||
        !Number(Get(h, "content-length"), length) ||
        total > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - trailerBytes || resumeOffset>total || (resumeOffset&&resumeKey.empty()) || length != (probe ? 0 : total-resumeOffset + trailerBytes) ||
        (!browser && !probe && Get(h, "expect") != "100-continue") || !Get(h, "transfer-encoding").empty() ||
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
    bool matched = false;std::wstring copyError;
    if (copying || probe || directory) {
        // A probe is authorized by the accepted item, but does not consume it when data is needed.
        const auto key = std::string(browser ? "web:" : "native:") + ip + ":" + Get(h, browser ? "x-chuan-client" : "x-chuan-id") + ":" + Get(h, "x-chuan-batch");
        uint64_t index = 0; bool authorized = false,cancelled=false;
        { std::lock_guard lock(mutex_); auto it = batchApprovals_.find(key);
          if (Number(Get(h,"x-chuan-item"),index) && it != batchApprovals_.end() && index < it->second.items.size()) {
              auto &batch = it->second; const auto &item = batch.items[index];
              cancelled=batch.cancelledItems.contains(static_cast<size_t>(index));
              authorized = item.folder && index < batch.selection.size() && batch.selection[index]=='1' && !batch.cancelledItems.contains(index) &&
                  batch.target == Get(h,"x-chuan-target") && batch.recipient == Get(h,"x-chuan-recipient") &&
                  (name.starts_with(item.name + L"\\") || (directory && name == item.name)) && total <= item.total;
              batch.touched = Now();
          }
        }
        if(cancelled){Reply(s,410,"Gone",Utf8(L"接收端已取消本次传输"));return;}
        if (!copying || !authorized || (directory && (!probe || total))) { Reply(s,409,"Conflict",Utf8(L"目录复制需要有效的批次授权")); return; }
        const bool metadata=probe&&Get(h,"x-chuan-metadata")=="1"&&!directory;
        if(metadata){
            const auto destination=cfg.receiveDir/name;
            const auto attributes=GetFileAttributesW(destination.c_str());
            if(!SafeParents(cfg.receiveDir,destination.parent_path()) ||
                (attributes!=INVALID_FILE_ATTRIBUTES&&((attributes&FILE_ATTRIBUTE_REPARSE_POINT)||(!staging&&recipientId.empty()&&(attributes&FILE_ATTRIBUTE_DIRECTORY))))){
                Reply(s,409,"Conflict",Utf8(L"目标路径类型冲突，原文件已保留"));return;
            }
            const bool exists=!staging&&recipientId.empty()&&attributes!=INVALID_FILE_ATTRIBUTES;
            if(exists){WIN32_FILE_ATTRIBUTE_DATA info{};if(!GetFileAttributesExW(destination.c_str(),GetFileExInfoStandard,&info)||
                ((static_cast<uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow)!=total){Reply(s,409,"Conflict",Utf8(L"同名文件大小不同，原文件已保留"));return;}}
            {
                std::lock_guard lock(mutex_);
                const auto batch=batchApprovals_.find(key);
                if(batch!=batchApprovals_.end())batch->second.touched=Now();
                for(const auto &candidate:jobs_)if(candidate->announced&&!candidate->cancel&&candidate->view.state<State::Completed&&
                    candidate->batchId==Get(h,"x-chuan-batch")&&candidate->webUpload==Get(h,"x-chuan-upload")&&
                    (browser?candidate->webClient:candidate->nativeOwner)==Get(h,browser?"x-chuan-client":"x-chuan-id")){
                    candidate->view.activity=exists?L"等待发送端核对同名文件":L"准备接收文件";TraceJobLocked(candidate);break;
                }
            }
            Reply(s,200,"OK",exists?"{\"verify\":true}":"{\"upload\":true}","application/json");return;
        }
        std::shared_ptr<Job> probing;
        try {
            const auto destination = cfg.receiveDir / name;
            if (!SafeParents(cfg.receiveDir, destination.parent_path())) throw std::runtime_error("unsafe parent");
            const auto attributes = GetFileAttributesW(destination.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("unsafe target");
            if (probe && directory) {
                if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("type conflict");
                matched = true;
            } else if (probe && !staging && recipientId.empty() && attributes != INVALID_FILE_ATTRIBUTES) {
                if (attributes & FILE_ATTRIBUTE_DIRECTORY) throw std::runtime_error("type conflict");
                WinFile existing(CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                LARGE_INTEGER size{};
                if (existing.value==INVALID_HANDLE_VALUE || !GetFileSizeEx(existing.value,&size) || static_cast<uint64_t>(size.QuadPart)!=total) throw std::runtime_error("content conflict");
                {
                    std::lock_guard lock(mutex_);
                    for (const auto &candidate : jobs_) if (candidate->announced && !candidate->cancel &&
                        candidate->view.state < State::Completed && candidate->view.name == name &&
                        candidate->batchId == Get(h,"x-chuan-batch") && candidate->webUpload == Get(h,"x-chuan-upload") &&
                        (browser ? candidate->webClient : candidate->nativeOwner) == Get(h,browser?"x-chuan-client":"x-chuan-id")) {
                        probing = candidate; probing->view.activity = L"正在校验已有文件";TraceJobLocked(probing); break;
                    }
                }
                Hash hash; std::vector<char> buffer(256*1024); DWORD count=0; uint64_t tick=Now();
                for (;;) {
                    if (!running_ || (probing && probing->cancel) || !ReadFile(existing.value,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr)) throw std::runtime_error("read");
                    if (!count) break; hash.Add(buffer.data(),count);
                    if (Now()-tick>2000) { if (!SendAll(s,"HTTP/1.1 102 Processing\r\n\r\n")) throw std::runtime_error("connection"); tick=Now(); }
                }
                const auto digest=hash.Finish(); matched=Hex(std::string(reinterpret_cast<const char*>(digest.data()),digest.size()))==Get(h,"x-chuan-sha256");
                if (!matched) throw std::runtime_error("content conflict");
            }
            if (probe && !matched) { Reply(s,200,"OK","{\"upload\":true}","application/json"); return; }
        } catch (...) { copyError=L"同名文件内容不同、类型冲突或无法安全核对；原文件已保留"; }
        if (probing) {
            std::lock_guard lock(mutex_);
            if (probing->view.activity == L"正在校验已有文件") probing->view.activity.clear();TraceJobLocked(probing);
        }
    }
    std::shared_ptr<Job> j;
    if (!Get(h,"x-chuan-batch").empty()) {
        std::lock_guard lock(mutex_);
        const auto owner=Get(h,browser?"x-chuan-client":"x-chuan-id");
        const auto approved = batchApprovals_.find(std::string(browser?"web:":"native:") + ip + ":" + owner + ":" + Get(h, "x-chuan-batch"));
        uint64_t item=0;
        if (approved!=batchApprovals_.end() && Number(Get(h,"x-chuan-item"),item) && approved->second.cancelledItems.contains(static_cast<size_t>(item))) {
            Reply(s,410,"Gone",Utf8(L"接收端已取消本次传输"));return;
        }
        for (auto &candidate : jobs_) if (candidate->announced && (browser?candidate->webClient:candidate->nativeOwner) == owner &&
            candidate->webUpload == Get(h, "x-chuan-upload") && candidate->batchId == Get(h, "x-chuan-batch") &&
            candidate->view.name == name && candidate->view.total == total && candidate->view.state == State::Queued) {
            j = candidate; j->announced = false; j->view.activity.clear();j->browserPaused=false;j->resumeActivity.clear(); break;
        }
        if (!j && approved != batchApprovals_.end() && approved->second.announcedItems.contains(static_cast<size_t>(item))) { Reply(s, 409, "Conflict", Utf8(L"文件与已登记的批次清单不一致")); return; }
    }
    if (!j) j = NewJob(name, sender + L" (" + Wide(ip) + L")", total, true);
    j->view.directory = directory;
    if (!browser) { j->nativeOwner=Get(h,"x-chuan-id");j->webUpload=Get(h,"x-chuan-upload"); }
    j->staging = staging || !recipientId.empty();
    j->targetId = target; j->senderName = sender;
    if (browser) {
        std::lock_guard lock(mutex_);
        j->staging = staging;
        j->webClient = Get(h, "x-chuan-client");
        j->webTab = Get(h, "x-chuan-tab");
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
                  valid = (item.folder ? (name.starts_with(item.name + L"\\") || (directory && name == item.name)) : name == item.name) &&
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
            !(name.starts_with(folderName + L"\\") || (directory && name == folderName)) || !Number(Get(h, "x-chuan-folder-count"), count) || !count ||
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
    if(!copyError.empty()){SetState(j,State::Failed,copyError);Reply(s,409,"Conflict",Utf8(copyError));return;}
    SetState(j, State::Waiting);
    if (!cfg.autoReceive && !staging && recipientId.empty()) {
        std::unique_lock lock(mutex_);
        j->view.awaitingApproval = j->decision == 0;
        if (!j->folderKey.empty() && !j->decision) {
            auto &batch = folderApprovals_.at(j->folderKey);
            if (!batch.prompt) batch.prompt = j->view.id;
            j->view.awaitingApproval = batch.prompt == j->view.id;
        }
        while (j->decision == 0 && !j->cancel && running_) {
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
            Reply(s, 417, "Expectation Failed", Utf8(j->cancel ? L"发送已取消或连接已中断" : L"接收请求已拒绝"));
            return;
        }
    }
    { std::lock_guard lock(mutex_); j->view.awaitingApproval = false; }
    if (j->cancel) {
        SetState(j, State::Cancelled);
        Reply(s, 417, "Expectation Failed");
        return;
    }
    if (probe && matched) {
        const auto destination=cfg.receiveDir / name;
        if (directory && !SafeParents(cfg.receiveDir,destination)) { SetState(j,State::Failed,L"无法创建目录"); Reply(s,409,"Conflict"); return; }
        if (!directory) RememberFile(j,destination);
        { std::lock_guard lock(mutex_); j->view.skipped=!directory; j->view.done=total;
          if (directory) j->view.savedPath=destination;
          if (browser || !recipientId.empty()) webFiles_[j->view.id]={destination,cfg.receiveDir,total,"",j,j->webClient,j->fileIdentity,j->hasIdentity,name};
        }
        if (!recipientId.empty()) {
            QueueBrowser(browserPeer,destination,j,cfg.receiveDir);
            uint64_t tick=Now();std::unique_lock lock(mutex_);
            while(running_&&!j->cancel&&j->view.state<State::Completed){j->cv.wait_for(lock,std::chrono::milliseconds(250));if(Now()-tick>2000){lock.unlock();bool live=SendAll(s,"HTTP/1.1 102 Processing\r\n\r\n");lock.lock();tick=Now();if(!live)break;}}
            if(j->view.state!=State::Completed){const auto reason=Utf8(j->view.error);lock.unlock();Reply(s,409,"Conflict",reason);return;}
        }
        else SetState(j,State::Completed);
        Reply(s,208,"Already Reported","{\"id\":"+Json(std::to_string(j->view.id))+",\"skipped\":true}","application/json"); return;
    }
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(cfg.receiveDir.c_str(), &available, nullptr, nullptr) ||
        total-resumeOffset > available.QuadPart || available.QuadPart - (total-resumeOffset) < 2 * 1024 * 1024) {
        Reply(s, 507, "Insufficient Storage");
        return;
    }
    diagnostic_->Write("\"event\":\"receive_space\",\"id\":"+std::to_string(j->view.id)+",\"available_bytes\":"+std::to_string(available.QuadPart)+",\"remaining_bytes\":"+std::to_string(total-resumeOffset)+",\"resume_offset\":"+std::to_string(resumeOffset));
    fs::path destination = cfg.receiveDir / fs::path(name),
             partial = resumeKey.empty()?cfg.receiveDir / (L".chuan-" + Guid() + L".part"):ResumePath(cfg.receiveDir,Get(h,browser?"x-chuan-client":"x-chuan-id"),name,total,resumeKey);
    uint64_t browserHashMs = 0;
    bool hashTimingValid = Number(Get(h, "x-chuan-hash-ms"), browserHashMs) && browserHashMs <= 604800000;
    diagnostic::Transfer trace(*diagnostic_, s, j->view.id, total, browser ? "web_upload" : "native_upload",
        ",\"client_id\":"+Json(j->webClient)+",\"recipient_id\":"+Json(j->recipient)+",\"batch_id\":"+Json(j->batchId)+",\"upload_id\":"+Json(j->webUpload)+",\"path\":"+Json(Utf8(j->view.name))+",\"staging\":" + std::string(staging ? "true" : "false") +
        ",\"checksum\":\"" + (verifyChecksum ? "sha256" : "none") + "\"" +
        (browser && hashTimingValid ? ",\"browser_hash_ms\":" + std::to_string(browserHashMs) : ""));
    try {
        if (!SafeParents(cfg.receiveDir, destination.parent_path()))
            throw std::runtime_error("unsafe path");
        {
            WinFile file(CreateFileW(
                partial.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, resumeKey.empty()?CREATE_NEW:OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (file.value == INVALID_HANDLE_VALUE)
                throw std::runtime_error("create file");
            BY_HANDLE_FILE_INFORMATION partialInfo{};LARGE_INTEGER existingSize{};
            if(!GetFileInformationByHandle(file.value,&partialInfo)||(partialInfo.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||partialInfo.nNumberOfLinks!=1||!GetFileSizeEx(file.value,&existingSize)||static_cast<uint64_t>(existingSize.QuadPart)!=resumeOffset)throw std::runtime_error("resume_offset");
            if(!resumeOffset&&(partialInfo.dwFileAttributes&FILE_ATTRIBUTE_COMPRESSED)){USHORT compression=COMPRESSION_FORMAT_NONE;DWORD returned=0;if(!DeviceIoControl(file.value,FSCTL_SET_COMPRESSION,&compression,sizeof(compression),nullptr,0,&returned,nullptr)){trace.fileError=GetLastError();throw std::runtime_error("disable_compression");}}
            std::unique_ptr<Hash> hash;
            if (verifyChecksum||resumeOffset) hash = std::make_unique<Hash>();
            if(resumeOffset){
                {std::lock_guard lock(mutex_);j->view.activity=L"核验已接收部分，准备续传";TraceJobLocked(j);}
                std::vector<char> prefix(1024*1024);uint64_t checked=0,tick=Now();while(checked<resumeOffset){if(j->cancel||j->paused||!running_)throw std::runtime_error("cancel");DWORD read=0;if(!ReadFile(file.value,prefix.data(),static_cast<DWORD>(std::min<uint64_t>(prefix.size(),resumeOffset-checked)),&read,nullptr)||!read)throw std::runtime_error("resume_read");hash->Add(prefix.data(),read);checked+=read;if(!browser&&Now()-tick>2000){if(!SendAll(s,"HTTP/1.1 102 Processing\r\n\r\n"))throw std::runtime_error("connection");tick=Now();}}
                if(hash->Snapshot()!=Get(h,"x-chuan-prefix"))throw std::runtime_error("resume_mismatch");
                if(!verifyChecksum)hash.reset();
            }
            if (!browser && !SendAll(s, "HTTP/1.1 100 Continue\r\n\r\n"))
                throw std::runtime_error("connection");
            j->progressBase=resumeOffset;SetState(j, State::Running);trace.StartAt(resumeOffset);
            std::vector<char> buffer(256 * 1024);
            uint64_t done = resumeOffset;Progress(j,done);
            while (done < total) {
                if (j->cancel || j->paused || !running_)
                    throw std::runtime_error("cancel");
                size_t want = static_cast<size_t>(std::min<uint64_t>(buffer.size(), total - done));
                int n;
                { diagnostic::Measure time(trace.networkMs);
                  n = recv(s, buffer.data(), static_cast<int>(want), 0);
                  if (n <= 0) { trace.socketError = n < 0 ? WSAGetLastError() : 0; throw std::runtime_error("connection"); } }
                DWORD written = 0;
                { diagnostic::Measure time(trace.diskMs);
                  const BOOL ok=WriteFile(file.value, buffer.data(), static_cast<DWORD>(n), &written, nullptr);
                  const DWORD error=ok?0:GetLastError();
                  if (!ok||written!=static_cast<DWORD>(n)){trace.fileError=error;throw std::runtime_error("disk");} }
                if (hash) { diagnostic::Measure time(trace.hashMs); hash->Add(buffer.data(), n); }
                done += static_cast<uint64_t>(n);
                Progress(j, done);
                trace.done = done; trace.Sample();
            }
            if (hash) {
                { std::lock_guard lock(mutex_); j->view.activity = L"正在校验接收内容";TraceJobLocked(j); }
                std::array<unsigned char, 32> expected{};
                { diagnostic::Measure time(trace.networkMs);
                  if (!ReadExact(s, expected.data(), expected.size())) { trace.socketError = WSAGetLastError(); throw std::runtime_error("checksum_read"); } }
                { diagnostic::Measure time(trace.hashMs);
                  if (hash->Finish() != expected) throw std::runtime_error("checksum"); }
            }
            if (j->cancel || !running_)
                throw std::runtime_error("cancel");
            { std::lock_guard lock(mutex_); j->view.activity = L"正在保存文件";TraceJobLocked(j); }
            { diagnostic::Measure time(trace.flushMs);
              if (!FlushFileBuffers(file.value)) { trace.fileError = GetLastError(); throw std::runtime_error("flush"); } }
        }
        fs::path chosen;
        for (unsigned i = 0; i < 10000; ++i) {
            chosen = copying && !staging && recipientId.empty() ? destination : UniquePath(destination, i);
            if (MoveFileExW(partial.c_str(), chosen.c_str(), MOVEFILE_WRITE_THROUGH))
                break;
            DWORD error = GetLastError();
            if ((copying && !staging && recipientId.empty()) || (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS))
                throw std::runtime_error("rename");
            chosen.clear();
        }
        if (chosen.empty())
            throw std::runtime_error("duplicates");
        RememberFile(j, chosen);
        trace.result = verifyChecksum ? "verified_and_saved" : "length_checked_and_saved";
        {
            std::lock_guard lock(mutex_);
            j->view.savedPath = chosen;
            if (browser) webFiles_[j->view.id] = {chosen, cfg.receiveDir, total, {}, {}, Get(h, "x-chuan-client"), j->fileIdentity, j->hasIdentity, name};
        }
        if (!recipientId.empty()) {
            QueueBrowser(browserPeer, chosen, j, cfg.receiveDir);
            SendAll(s, "HTTP/1.1 102 Processing\r\n\r\n");
            uint64_t keepalive = Now();
            std::unique_lock lock(mutex_);
            while (running_ && !j->cancel && j->view.state < State::Completed) {
                j->cv.wait_for(lock, std::chrono::milliseconds(250));
                if (Now() - keepalive > 2000) {
                    lock.unlock(); bool live = SendAll(s, "HTTP/1.1 102 Processing\r\n\r\n"); lock.lock();
                    keepalive = Now(); if (!live) break;
                }
            }
            bool complete = j->view.state == State::Completed;
            const auto reason=Utf8(j->view.error);
            lock.unlock(); Reply(s, complete ? 201 : 409, complete ? "Created" : "Conflict",reason); return;
        }
        SetState(j, State::Completed);
        if (browser)
            Reply(s, 201, "Created", "{\"id\":" + Json(std::to_string(j->view.id)) + "}",
                  "application/json; charset=utf-8");
        else
            Reply(s, 201, "Created", Utf8(chosen.filename().wstring()));
    } catch (const std::exception &ex) {
        trace.result = j->cancel || !running_ ? "cancelled" : "failed";
        trace.failureStage = ex.what();
        if(resumeKey.empty()||std::string(ex.what())=="checksum")DeleteFileW(partial.c_str());
        diagnostic_->Write("\"event\":\"resume_retained\",\"id\":"+std::to_string(j->view.id)+",\"path\":"+Json(Utf8(partial.wstring()))+",\"offset\":"+std::to_string(j->view.done));
        std::wstring reason = j->cancel ? L"接收端已取消本次传输" : std::string(ex.what()) == "checksum" ? L"完整性校验失败"
                              : std::string(ex.what()) == "disk"   ? L"写入接收目录失败"
                                                                   : L"传输中断，未保存正式文件";
        if(std::string(ex.what())=="resume_mismatch")reason=L"续传内容不一致，需从头接收该文件";
        if(trace.fileError==ERROR_DISK_FULL)reason=L"接收目录写入失败（系统错误 112），已保留续传数据";
        SetState(j,j->paused?State::Failed:j->cancel || !running_ ? State::Cancelled : State::Failed,j->paused?L"":reason);
        {std::lock_guard lock(mutex_);if(j->paused)j->view.activity=L"已暂停，已接收数据已保留";}
        Reply(s,std::string(ex.what())=="resume_mismatch"?409:422,"Unprocessable Content",Utf8(reason));
    }
}
void Engine::SendOne(const Peer &peer, const fs::path &path, const std::wstring &relative,
                     const std::shared_ptr<Job> &j, const fs::path &webRoot, SendStage stage) {
    const bool archiveStream=!!j->archivePlan;
    const bool preparation = stage==SendStage::Prepare||stage==SendStage::Probe||stage==SendStage::Verify;
    if(j->paused){SetState(j,State::Failed);return;}
    if(stage!=SendStage::Transfer)j->verificationPending=false;
    if (peer.browser && peer.owner.empty()) {
        if (preparation) return;
        QueueBrowser(peer, path, j, webRoot);
        return;
    }
    if (j->cancel || !running_) {
        SetState(j, State::Cancelled);
        return;
    }
    if(preparation&&j->folderId.empty())return;
    WinFile file(preparation||archiveStream?INVALID_HANDLE_VALUE:CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN | (j->view.directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
    LARGE_INTEGER size{};
    if (!preparation&&!archiveStream&&(file.value == INVALID_HANDLE_VALUE || (!j->view.directory && !GetFileSizeEx(file.value, &size)))) {
        SetState(j, State::Failed, L"无法读取文件，文件可能正在被修改");
        return;
    }
    if (!preparation&&!archiveStream&&!j->view.directory && !webRoot.empty() && (!SafeWebPath(path, webRoot) || !WebHandleMatches(file.value, path) ||
                             static_cast<uint64_t>(size.QuadPart) != j->view.total)) {
        SetState(j, State::Failed, L"文件路径已变更，无法转发");
        return;
    }
    uint64_t total = preparation||archiveStream?j->view.total:static_cast<uint64_t>(size.QuadPart);
    if(!preparation&&!archiveStream)RememberFile(j, path);
    if (total > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 32) {
        SetState(j, State::Failed, L"文件过大");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        j->view.total = total;
    }
    std::string digest;
    {std::lock_guard lock(mutex_);j->view.activity=L"连接接收端";TraceJobLocked(j);}
    SOCKET s = Connect(peer.gateway.empty() ? peer.ip : peer.gateway, peer.port);
    if (s == INVALID_SOCKET) {
        SetState(j, State::Failed, L"无法连接目标设备");
        return;
    }
    TrackSocket(s, j);
    SetTimeout(s, 150000, 30000);
    std::unique_ptr<diagnostic::Transfer> trace;
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
            "\r\nX-Chuan-Server: " + Utf8(peer.owner.empty() ? peer.id : peer.owner) +
            "\r\nX-Chuan-Platform: windows\r\nX-Chuan-Port: " + std::to_string(boundPort_) + "\r\nX-Chuan-Path: " + Hex(Utf8(relative)) +
            "\r\nX-Chuan-Sender: " + Hex(Utf8(sender)) + "\r\nX-Chuan-Size: " + std::to_string(total) +
            "\r\nX-Chuan-Recipient: " + (peer.browser ? Hex(Utf8(peer.id)) : "") +
            "\r\nX-Chuan-Display-Sender: " + Hex(Utf8(j->senderName)) +
            "\r\nX-Chuan-Batch: " + j->batchId + "\r\nX-Chuan-Item: " + std::to_string(j->batchItem) +
            "\r\nX-Chuan-Upload: " + j->webUpload +
            "\r\nX-Chuan-Folder: " + j->folderId + "\r\nX-Chuan-Folder-Name: " + Hex(Utf8(j->view.folderName)) +
            "\r\nX-Chuan-Folder-Count: " + std::to_string(j->view.folderCount) + "\r\nX-Chuan-Folder-Size: " + std::to_string(j->view.folderTotal) +
            "\r\nContent-Length: " + std::to_string(total + 32) + "\r\n\r\n";
        if (!j->folderId.empty() && stage != SendStage::Transfer) {
            const auto reconnect=[&]{ReleaseSocket(s);s=Connect(peer.gateway.empty()?peer.ip:peer.gateway,peer.port);if(s==INVALID_SOCKET)throw std::runtime_error("connect");TrackSocket(s,j);SetTimeout(s,150000,30000);};
            const auto probe=[&](const std::string &endpoint,std::string &body){
                auto query=request;query.replace(query.find("/api/upload"),11,endpoint);query.erase(query.find("Expect: 100-continue\r\n"),22);
                query.replace(query.find("Content-Length:"),std::string::npos,"X-Chuan-Copy: 1\r\nX-Chuan-Directory: "+std::string(j->view.directory?"1":"0")+"\r\nX-Chuan-Sha256: "+digest+"\r\nContent-Length: 0\r\n\r\n");
                if(!SendAll(s,query))throw std::runtime_error("probe");
                std::string reply;std::map<std::string,std::string> fields;
                do{fields.clear();if(!ReadHeader(s,reply,fields))throw std::runtime_error("probe");}while(reply.starts_with("HTTP/1.1 102 "));
                uint64_t length=0;if(Number(Get(fields,"content-length"),length)&&length&&length<=131072){body.assign(static_cast<size_t>(length),'\0');if(!ReadExact(s,body.data(),body.size()))throw std::runtime_error("probe");}
                return reply;
            };
            {std::lock_guard lock(mutex_);j->view.activity=L"检查接收位置";TraceJobLocked(j);}
            std::string body;auto reply=stage==SendStage::Verify?std::string("HTTP/1.1 200 OK"):probe("/api/copy-check",body);
            const bool legacy=reply.starts_with("HTTP/1.1 404 ")||reply.starts_with("HTTP/1.1 405 ");
            if(stage==SendStage::Verify||legacy||body.find("\"verify\":true")!=std::string::npos){
                if(stage==SendStage::Probe){j->verificationPending=true;ReleaseSocket(s);SetState(j,State::Queued);{std::lock_guard lock(mutex_);j->view.activity=L"等待核对同名文件";TraceJobLocked(j);}return;}
                ReleaseSocket(s);s=INVALID_SOCKET;
                if(!j->view.directory){
                    {std::lock_guard lock(mutex_);j->view.activity=L"核对同名文件";TraceJobLocked(j);}
                    if(preparation){
                        file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
                        LARGE_INTEGER actual{};
                        if(file.value==INVALID_HANDLE_VALUE||!GetFileSizeEx(file.value,&actual)||static_cast<uint64_t>(actual.QuadPart)!=total)throw std::runtime_error("source changed");
                    }
                    Hash content;std::vector<char> data(256*1024);DWORD count=0;uint64_t tick=Now();
                    for(;;){
                        if(j->cancel||j->retryExpired||!running_)throw std::runtime_error("cancel");
                        if(!ReadFile(file.value,data.data(),static_cast<DWORD>(data.size()),&count,nullptr))throw std::runtime_error("read");
                        if(!count)break;content.Add(data.data(),count);
                        if(Now()-tick>2000){std::string status;if(BatchCommand(peer,"/api/upload-status/"+j->webUpload,j->batchId,"",status,j)==200&&status=="4"){j->cancel=true;throw std::runtime_error("cancel");}tick=Now();}
                    }
                    const auto bytes=content.Finish();digest=Hex(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
                    LARGE_INTEGER zero{};if(!SetFilePointerEx(file.value,zero,nullptr,FILE_BEGIN))throw std::runtime_error("seek");
                }
                reconnect();{std::lock_guard lock(mutex_);j->view.activity=L"等待接收端核对同名文件";TraceJobLocked(j);}
                body.clear();reply=probe("/api/copy-probe",body);
            }
            if(reply.starts_with("HTTP/1.1 208 ")){Progress(j,total);{std::lock_guard lock(mutex_);j->view.skipped=!j->view.directory;j->view.activity.clear();}SetState(j,State::Completed);ReleaseSocket(s);return;}
            if(reply.starts_with("HTTP/1.1 410 ")){j->cancel=true;SetState(j,State::Cancelled,L"接收端已取消本次传输");ReleaseSocket(s);return;}
            if(reply.starts_with("HTTP/1.1 417 ")){SetState(j,State::Rejected,L"接收端已拒绝本次传输");ReleaseSocket(s);return;}
            if(!reply.starts_with("HTTP/1.1 200 ")){SetState(j,State::Failed,L"同名文件内容不同或接收端不支持目录补传；原文件已保留");ReleaseSocket(s);return;}
            if(preparation){ReleaseSocket(s);SetState(j,State::Queued);{std::lock_guard lock(mutex_);j->view.activity.clear();}return;}
            reconnect();
            request.insert(request.find("Content-Length:"),"X-Chuan-Copy: 1\r\n");
            {std::lock_guard lock(mutex_);j->view.activity.clear();}
        }
        if(stage==SendStage::Transfer&&!j->folderId.empty())request.insert(request.find("Content-Length:"),"X-Chuan-Copy: 1\r\n");
        uint64_t resumeOffset=0;std::unique_ptr<Hash> contentHash=std::make_unique<Hash>();
        if(!j->view.directory){
            std::string key;if(archiveStream){Hash metadata;for(const auto &entry:j->archivePlan->entries){const auto data=entry.name+":"+std::to_string(entry.size)+"\n";metadata.Add(data.data(),data.size());}key=metadata.Snapshot();}else key=ResumeKey(file.value,total);
            auto query=request;query.replace(query.find("/api/upload"),11,"/api/resume");query.erase(query.find("Expect: 100-continue\r\n"),22);query.replace(query.find("Content-Length:"),std::string::npos,"X-Chuan-Resume-Key: "+key+"\r\nX-Chuan-Resume-Reset: "+std::string(j->resumeReset?"1":"0")+"\r\nContent-Length: 0\r\n\r\n");
            std::string reply,body;std::map<std::string,std::string> fields;if(!SendAll(s,query)||!ReadHeader(s,reply,fields))throw std::runtime_error("resume_probe");uint64_t bytes=0;if(Number(Get(fields,"content-length"),bytes)&&bytes<=4096){body.resize(static_cast<size_t>(bytes));if(!ReadExact(s,body.data(),body.size()))throw std::runtime_error("resume_probe");}
            const bool supported=reply.starts_with("HTTP/1.1 200 ");if(supported){const auto at=body.find(':');if(at==std::string::npos)throw std::runtime_error("resume_probe");const auto end=body.find_first_not_of("0123456789",at+1);if(!Number(body.substr(at+1,end-at-1),resumeOffset)||resumeOffset>total)throw std::runtime_error("resume_offset");j->resumeReset=false;
                if(resumeOffset){
                    {std::lock_guard lock(mutex_);j->view.activity=L"核验已接收部分，准备续传";TraceJobLocked(j);}
                    if(archiveStream){struct PrefixDone{};uint64_t checked=0;try{archive::Emit(*j->archivePlan,[&](const void *data,size_t count){const auto take=static_cast<size_t>(std::min<uint64_t>(count,resumeOffset-checked));contentHash->Add(data,take);checked+=take;if(j->retryDeadline)j->retryDeadline=Now()+30000;if(checked==resumeOffset)throw PrefixDone{};},[](uint64_t,const std::wstring &){},[&]{return j->cancel||j->paused||!running_;});}catch(const PrefixDone&){}if(checked!=resumeOffset)throw std::runtime_error("resume_read");}
                    else{std::vector<char> prefix(1024*1024);uint64_t checked=0;while(checked<resumeOffset){if(j->cancel||j->paused||!running_)throw std::runtime_error("cancel");DWORD read=0;if(!ReadFile(file.value,prefix.data(),static_cast<DWORD>(std::min<uint64_t>(prefix.size(),resumeOffset-checked)),&read,nullptr)||!read)throw std::runtime_error("无法读取源文件");contentHash->Add(prefix.data(),read);checked+=read;if(j->retryDeadline)j->retryDeadline=Now()+30000;}}
                }
                request.insert(request.find("Content-Length:"),"X-Chuan-Resume-Key: "+key+"\r\nX-Chuan-Offset: "+std::to_string(resumeOffset)+"\r\nX-Chuan-Prefix: "+(resumeOffset?contentHash->Snapshot():"")+"\r\n");request.replace(request.find("Content-Length:"),std::string::npos,"Content-Length: "+std::to_string(total-resumeOffset+32)+"\r\n\r\n");
            }else if(!reply.starts_with("HTTP/1.1 404 "))throw std::runtime_error("resume_busy");
            ReleaseSocket(s);s=Connect(peer.gateway.empty()?peer.ip:peer.gateway,peer.port);if(s==INVALID_SOCKET)throw std::runtime_error("connect");TrackSocket(s,j);SetTimeout(s,150000,30000);
        }
        { std::lock_guard lock(mutex_); j->view.activity = L"等待接收端确认";TraceJobLocked(j); }
        if (!SendAll(s, request))
            throw std::runtime_error("request");
        std::string first;
        std::map<std::string, std::string> headers;
        do{headers.clear();if(!ReadHeader(s,first,headers))throw std::runtime_error("response");if(j->retryDeadline)j->retryDeadline=Now()+30000;}while(first.starts_with("HTTP/1.1 102 "));
        if (!first.starts_with("HTTP/1.1 100 ")) {
            if(first.starts_with("HTTP/1.1 409 ")){uint64_t n=0;std::string reason;if(Number(Get(headers,"content-length"),n)&&n<4096){reason.resize(static_cast<size_t>(n));ReadExact(s,reason.data(),reason.size());}if(reason.find("续传内容不一致")!=std::string::npos)j->resumeReset=true;}
            SetState(j, first.starts_with("HTTP/1.1 417") ? State::Rejected : State::Failed,
                     first.starts_with("HTTP/1.1 507") ? L"目标设备磁盘空间不足"
                                                       : L"目标设备拒绝接收或服务繁忙");
            ReleaseSocket(s);
            return;
        }
        SetTimeout(s);
        trace=std::make_unique<diagnostic::Transfer>(*diagnostic_,s,j->view.id,total,"native_send",",\"path\":"+Json(Utf8(j->view.name))+",\"batch_id\":"+Json(j->batchId)+",\"upload_id\":"+Json(j->webUpload)+",\"client_id\":"+Json(j->webClient)+",\"recipient_id\":"+Json(j->recipient));
        trace->StartAt(resumeOffset);j->progressBase=resumeOffset;
        auto hash=std::move(contentHash);
        { std::lock_guard lock(mutex_); j->view.activity = L"正在发送文件";TraceJobLocked(j); }
        SetState(j, State::Running);
        std::vector<char> buffer(256 * 1024);
        uint64_t done = resumeOffset;Progress(j,done);
        if(archiveStream){
            {std::lock_guard lock(mutex_);j->view.activity=L"边打包边发送 ZIP";TraceJobLocked(j);}
            diagnostic_->Write("\"event\":\"archive_stream_start\",\"id\":"+std::to_string(j->view.id)+",\"total_bytes\":"+std::to_string(total));
            uint64_t generated=0;archive::Emit(*j->archivePlan,[&](const void *data,size_t count){
                if(j->cancel||j->paused||j->retryExpired||!running_)throw std::runtime_error("cancel");
                const auto skip=static_cast<size_t>(std::min<uint64_t>(count,resumeOffset>generated?resumeOffset-generated:0));generated+=count;data=static_cast<const char*>(data)+skip;count-=skip;if(!count)return;
                if(hash){diagnostic::Measure time(trace->hashMs);hash->Add(data,count);}
                {diagnostic::Measure time(trace->networkMs);if(!SendAll(s,data,count))throw std::runtime_error("send");}
                done+=count;Progress(j,done);trace->done=done;trace->Sample();
            },[](uint64_t,const std::wstring &){},[&]{return j->cancel||j->retryExpired||!running_;});
        }else{
        while (done < total) {
            if (j->cancel || j->paused || !running_)
                throw std::runtime_error("cancel");
            DWORD count = 0;
            {diagnostic::Measure time(trace->diskMs);
            if (!ReadFile(file.value, buffer.data(),
                          static_cast<DWORD>(std::min<uint64_t>(buffer.size(), total - done)), &count,
                          nullptr) ||
                !count)
                {trace->fileError=GetLastError();throw std::runtime_error("file");}}
            if(hash){diagnostic::Measure time(trace->hashMs);hash->Add(buffer.data(), count);}
            {diagnostic::Measure time(trace->networkMs);if (!SendAll(s, buffer.data(), count))
                {trace->socketError=WSAGetLastError();throw std::runtime_error("send");}}
            done += count;
            Progress(j, done);trace->done=done;trace->Sample();
        }
        }
        std::string sentDigest;if(hash){const auto bytes=hash->Finish();sentDigest.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}else sentDigest=Unhex(digest);
        if (!SendAll(s, sentDigest.data(), sentDigest.size()))
            throw std::runtime_error("send");
        { std::lock_guard lock(mutex_); j->view.activity = L"等待接收端保存确认";TraceJobLocked(j); }
        headers.clear();
        if (!ReadHeader(s, first, headers)) throw std::runtime_error("confirmation");
        while (first.starts_with("HTTP/1.1 102 ")) {
            SetState(j, State::Waiting); headers.clear();
            if (j->cancel || !ReadHeader(s, first, headers)) throw std::runtime_error("confirmation");
        }
        if (!first.starts_with("HTTP/1.1 201 ")) {
            uint64_t length=0;std::string reason;
            if(Number(Get(headers,"content-length"),length)&&length<=131072){reason.resize(static_cast<size_t>(length));if(!ReadExact(s,reason.data(),reason.size()))reason.clear();}
            SetState(j,State::Failed,reason.empty()?L"目标设备未确认保存":Wide(reason));
            if(trace){trace->result="failed";trace->failureStage="confirmation";trace->Sample(true);}ReleaseSocket(s);return;
        }
        trace->result="confirmed_saved";SetState(j, State::Completed);
    } catch (const std::exception &error) {
        if(trace){trace->result=j->cancel?"cancelled":"failed";trace->failureStage=error.what();}
        SetState(j, j->cancel || !running_ ? State::Cancelled : State::Failed,
                 archiveStream?Wide(error.what()):L"连接中断或目标设备未确认保存");
    }
    if(trace)trace->Sample(true);ReleaseSocket(s);
}
void Engine::SendFiles(const Peer &peer, const std::vector<fs::path> &input,bool forceZip) {
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
        std::shared_ptr<archive::Plan> archive;
    };
    const auto prepareJob=[](const std::wstring &name,const std::wstring &peer,uint64_t size,bool receiving){auto job=std::make_shared<Job>();job->view.name=name;job->view.peer=peer;job->view.total=size;job->view.receiving=receiving;job->view.started=Now();return job;};
    std::vector<Item> files;
    std::vector<OfferItem> manifest;
    Config cfg; { std::lock_guard lock(mutex_); cfg = config_; }
    cfg.zipFolders=cfg.zipFolders||forceZip;
    uint64_t batchBytes = 0,sourceCount=0;std::set<std::wstring> archiveNames;
    try {
        for(const auto &path:input)if(!fs::is_directory(path))archiveNames.insert(path.filename().wstring());
        for (auto &source : input) {
            if (fs::is_symlink(source)) {
                Notice(L"不支持发送符号链接");
                return;
            }
            if (fs::is_regular_file(source)) {
                auto rel = SanitizeRelative(source.filename().wstring());
                if (rel.empty())
                    throw std::runtime_error("name");
                auto job = prepareJob(rel, peer.name, fs::file_size(source), false);
                job->batchItem = manifest.size(); manifest.push_back({rel, false, 1, job->view.total});
                files.push_back({source, rel, job,{}});++sourceCount;
            } else if (fs::is_directory(source) && !cfg.zipFolders) {
                const auto root=SanitizeRelative(source.filename().wstring());
                if(root.empty()) throw std::runtime_error("name");
                const auto index=manifest.size(); GUID folderGuid{}; CoCreateGuid(&folderGuid); const auto folder=Hex(std::string(reinterpret_cast<char*>(&folderGuid),sizeof(folderGuid)));
                std::vector<fs::path> entries{source};
                for(const auto &entry:fs::recursive_directory_iterator(source)) entries.push_back(entry.path());
                uint64_t bytes=0; const auto start=files.size();
                for(const auto &entry:entries){
                    if(GetFileAttributesW(entry.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("link");
                    const bool dir=fs::is_directory(entry); if(!dir&&!fs::is_regular_file(entry))throw std::runtime_error("type");
                    const auto rel=entry==source?root:SanitizeRelative((fs::path(root)/entry.lexically_relative(source)).wstring());
                    const auto size=dir?0:fs::file_size(entry); if(size>UINT64_MAX-bytes)throw std::runtime_error("overflow");bytes+=size;
                    auto job=prepareJob(rel,peer.name,size,false);job->view.directory=dir;job->archiveSource=source;job->batchItem=index;
                    job->folderId=folder;job->view.folderId=folder;job->view.folderName=root;
                    files.push_back({entry,rel,job,{}});++sourceCount;
                }
                manifest.push_back({root,true,entries.size(),bytes});
                for(size_t i=start;i<files.size();++i){files[i].job->view.folderCount=entries.size();files[i].job->view.folderTotal=bytes;}
            } else if (fs::is_directory(source)) {
                auto plan=std::make_shared<archive::Plan>(archive::Prepare(source));
                unsigned suffix=2;while(!archiveNames.insert(plan->name).second)plan->name=source.filename().wstring()+L" ("+std::to_wstring(suffix++)+L").zip";
                auto job=prepareJob(plan->name,peer.name,plan->size,false);job->archiveSource=source;job->archivePlan=plan;
                job->batchItem=manifest.size();manifest.push_back({plan->name,false,1,plan->size});
                files.push_back({source,plan->name,job,plan});sourceCount+=std::max<uint64_t>(1,plan->count);
            } else
                throw std::runtime_error("missing");
            if (cfg.maxTransferFiles && sourceCount > cfg.maxTransferFiles) throw std::runtime_error("limit");
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
    {std::lock_guard lock(mutex_);for(auto &file:files){file.job->view.id=nextJob_++;jobs_.push_back(file.job);}}
    for (auto &file : files) {
        if(!file.archive)RememberFile(file.job, file.path);
        std::lock_guard lock(mutex_); file.job->targetId = peer.id; file.job->view.canRetry = true;
    }
    auto pending = files;
    bool ok = Launch([this, peer, cfg, files = std::move(files), manifest = std::move(manifest)] {
        GUID value{}; CoCreateGuid(&value); const auto batch = Hex(std::string(reinterpret_cast<const char *>(&value), sizeof(value)));
        auto control=std::make_shared<Job>(); control->senderName=cfg.name;
        for (auto &file : files) {
            SetState(file.job, State::Waiting);
            std::lock_guard lock(mutex_); file.job->batchId=batch; GUID uploadId{}; CoCreateGuid(&uploadId);
            file.job->webUpload=Hex(std::string(reinterpret_cast<char*>(&uploadId),sizeof(uploadId)));
        }
        const bool declare=!peer.browser;
        std::set<size_t> declared;
        std::set<uint64_t> processed;
        const auto sendSelected=[&](const std::string &selection) {
            struct Work { const Item *file; fs::path path; bool registered; };
            std::vector<Work> work;
            for (auto &f : files) {
                const auto index=f.job->batchItem;
                if(selection[index]=='-'||!processed.insert(f.job->view.id).second)continue;
                if(selection[index]!='1'){SetState(f.job,f.job->cancel?State::Cancelled:State::Rejected,L"接收方未选择此项");continue;}
                bool registered=declared.contains(index);
                if(declare&&!registered&&!f.job->cancel){
                    std::string response,body;
                    for(const auto &entry:files)if(entry.job->batchItem==index)body+=std::to_string(index)+"\t"+entry.job->webUpload+"\t"+entry.job->folderId+"\t"+Hex(Utf8(entry.relative))+"\t"+std::to_string(entry.job->view.total)+(entry.archive?"\tA":entry.job->view.directory?"\tD":"")+"\n";
                    const auto status=BatchCommand(peer,"/api/batch-files",batch,body,response,f.job);registered=status==200;
                    if(registered)declared.insert(index);
                    if(!registered&&status!=404&&status!=405){SetState(f.job,State::Failed,L"接收端未确认传输清单");continue;}
                }
                SetState(f.job,State::Queued);work.push_back({&f,f.path,registered});
            }
            const auto finish=[&](Work &item){
                auto &f=*item.file;
                if(!running_)return;
                if(item.registered&&!f.job->cancel){State state;{std::lock_guard lock(mutex_);state=f.job->view.state;}
                    if(state>=State::Cancelled){std::string response;if(BatchCommand(peer,"/api/upload-status/"+f.job->webUpload,f.job->batchId,"",response,f.job)==200){
                        if(response=="4"){f.job->cancel=true;SetState(f.job,State::Cancelled,L"接收端已取消本次传输");}
                        else if(response=="3"){Progress(f.job,f.job->view.total);SetState(f.job,State::Completed);}}}}
                State state;std::wstring error;fs::path retryPath;
                {std::lock_guard lock(mutex_);state=f.job->view.state;error=f.job->view.error;
                    retryPath=f.job->archiveFile?f.job->archiveFile->path:f.path;
                    if(f.archive&&state!=State::Failed&&!(peer.browser&&peer.owner.empty())){f.job->archiveFile.reset();f.job->view.savedPath.clear();f.job->hasIdentity=false;}}
                if(state==State::Failed&&!f.job->cancel){QueueRetry(peer,retryPath,f.job);return;}
                if(item.registered&&state>=State::Cancelled){std::string response;BatchCommand(peer,"/api/batch-status",f.job->batchId,f.job->webUpload+"\t"+(state==State::Cancelled?"4":"5")+"\t"+Hex(Utf8(error.empty()?L"发送未完成":error))+"\n",response,{});}
            };
            const auto pipeline=[&](const char *phase,size_t index,int64_t ready){const auto &job=work[index].file->job;const bool deferred=std::string(phase)=="verification_queued";
                diagnostic_->Write("\"event\":\"pipeline\",\"phase\":"+Json(phase)+",\"batch_files\":"+std::to_string(work.size())+",\"index\":"+std::to_string(index)+",\"batch_id\":"+Json(batch)+",\"upload_id\":"+Json(job->webUpload)+",\"id\":"+std::to_string(job->view.id)+",\"client_id\":"+Json(job->webClient)+",\"recipient_id\":"+Json(job->recipient)+(deferred?",\"verification_pending\":":",\"ready\":")+std::to_string(ready)+",\"preparing\":"+std::to_string(diagnosticPreparing_.load())+",\"verifying\":"+std::to_string(diagnosticVerifying_.load())+",\"transferring\":"+std::to_string(diagnosticTransferring_.load())+",\"preparation_limit\":"+std::to_string(FileProbeLimit)+",\"verification_limit\":"+std::to_string(FileVerificationLimit)+",\"transfer_limit\":"+std::to_string(FileTransferLimit)+",\"ready_limit\":"+std::to_string(FileReadyLimit));};
            RunStagedFilePipeline(work.size(),[&](size_t index){
                auto &item=work[index];auto &f=*item.file;
                pipeline("probe_slot_wait",index,-1);preparationSlots_.acquire();++diagnosticPreparing_;pipeline("probe_start",index,-1);
                try{if(!f.archive)SendOne(peer,item.path,f.relative,f.job,{},SendStage::Probe);}
                catch(const std::exception &error){SetState(f.job,f.job->cancel||!running_?State::Cancelled:State::Failed,Wide(error.what()));}
                --diagnosticPreparing_;pipeline("probe_end",index,-1);preparationSlots_.release();
                bool active;{std::lock_guard lock(mutex_);active=f.job->view.state<State::Completed&&!f.job->cancel&&running_;}
                if(!active){finish(item);return PipelineRoute::Done;}
                if(f.job->verificationPending){std::lock_guard lock(mutex_);f.job->view.activity=f.archive?L"等待打包文件":L"等待核对同名文件";TraceJobLocked(f.job);return PipelineRoute::Verify;}
                SetState(f.job,State::Queued);{std::lock_guard lock(mutex_);if(f.job->view.state<State::Completed)f.job->view.activity=L"已就绪，等待传输";TraceJobLocked(f.job);}return PipelineRoute::Ready;
            },[&](size_t index){
                auto &item=work[index];auto &f=*item.file;
                pipeline("verification_slot_wait",index,-1);verificationSlots_.acquire();++diagnosticVerifying_;pipeline("verification_start",index,-1);
                try{
                    if(f.job->cancel||!running_)SetState(f.job,State::Cancelled);
                    else{
                        auto &path=item.path;
                        SendOne(peer,path,f.relative,f.job,{},SendStage::Verify);
                    }
                }catch(const std::exception &error){SetState(f.job,f.job->cancel||!running_?State::Cancelled:State::Failed,f.job->cancel?L"传输已取消":Wide(error.what()));}
                --diagnosticVerifying_;pipeline("verification_end",index,-1);verificationSlots_.release();
                bool ready;{std::lock_guard lock(mutex_);ready=f.job->view.state<State::Completed&&!f.job->cancel&&running_;}
                if(!ready){finish(item);return false;}
                SetState(f.job,State::Queued);{std::lock_guard lock(mutex_);if(f.job->view.state<State::Completed)f.job->view.activity=L"已就绪，等待传输";TraceJobLocked(f.job);}return true;
            },[&](size_t index){
                auto &item=work[index];auto &f=*item.file;
                pipeline("transfer_slot_wait",index,-1);transferSlots_.acquire();++diagnosticTransferring_;pipeline("transfer_start",index,-1);
                try{SendOne(peer,item.path,f.relative,f.job,{},SendStage::Transfer);}
                catch(const std::exception &error){SetState(f.job,f.job->cancel?State::Cancelled:State::Failed,Wide(error.what()));}
                --diagnosticTransferring_;pipeline("transfer_end",index,-1);transferSlots_.release();finish(item);
            },[&](const char *phase,size_t index,size_t ready){pipeline(phase,index,static_cast<int64_t>(ready));});
        };
        const auto cancelled=[&] {
            std::lock_guard lock(mutex_); std::string mask(manifest.size(),'-'); bool all=true;
            for(const auto &file:files) { if(file.job->cancel)mask[file.job->batchItem]='0';else all=false; }
            if(all)control->cancel=true;return mask;
        };
        std::string selection; std::wstring error;
        if (!SendOffer(peer,batch,manifest,selection,error,control,INVALID_SOCKET,sendSelected,cancelled))
            for(const auto &file:files) { bool active;{std::lock_guard lock(mutex_);active=file.job->view.state<State::Completed;}if(active)SetState(file.job,file.job->cancel?State::Cancelled:State::Failed,error); }

    });
    if (!ok) {
        for (auto &f : pending)
            SetState(f.job, State::Failed, L"服务繁忙");
        Notice(L"当前任务过多，请稍后发送");
    }
}
void Engine::QueueRetry(const Peer &peer,const fs::path &path,const std::shared_ptr<Job> &job){
    std::wstring terminal;{std::lock_guard lock(mutex_);if(PermanentTransferFailure(job->view.error))terminal=job->view.error;}
    if(!terminal.empty()){std::string response;BatchCommand(peer,"/api/batch-status",job->batchId,job->webUpload+"\t5\t"+Hex(Utf8(terminal))+"\n",response,{});return;}
    std::lock_guard lock(mutex_);if(job->cancel||job->retryDeadline||!running_)return;
    job->retryDeadline=Now()+30000;job->retryExpired=false;job->view.state=State::Waiting;job->view.activity=L"独立通道自动重试中";TraceJobLocked(job);job->view.error.clear();
    retryQueue_.push_back({peer,path,job});
    if(!retryThread_.joinable()){
        retryThread_=std::thread([this]{RetryLoop();});
        retryTimerThread_=std::thread([this]{while(running_){
            {std::lock_guard lock(mutex_);for(auto &item:jobs_)if(item->retryDeadline&&Now()>=item->retryDeadline&&!item->retryExpired&&!item->cancel&&!item->paused&&item->view.state!=State::Completed){
                item->retryExpired=true;item->view.state=State::Failed;item->view.activity.clear();item->view.error=L"传输失败超时（自动重试已达 30 秒）";
                std::lock_guard socketsLock(socketsMutex_);for(auto &[socket,weak]:sockets_)if(weak.lock()==item)shutdown(socket,SD_BOTH);
            }}std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }});
    }retryCv_.notify_one();
}
void Engine::RetryLoop(){
    while(running_){
        RetryItem item;{std::unique_lock lock(mutex_);retryCv_.wait(lock,[&]{return !running_||!retryQueue_.empty();});if(!running_)break;item=retryQueue_.front();retryQueue_.erase(retryQueue_.begin());}
        auto job=item.job;
        if(job->paused&&!job->remotePaused){job->retryDeadline=Now()+30000;std::unique_lock lock(mutex_);retryQueue_.push_back(item);if(std::all_of(retryQueue_.begin(),retryQueue_.end(),[](const auto &queued){return queued.job->paused&&!queued.job->remotePaused;}))retryCv_.wait_for(lock,std::chrono::milliseconds(250));continue;}
        if(job->remotePaused)job->retryDeadline=Now()+30000;
        if(job->cancel){job->retryDeadline=0;SetState(job,State::Cancelled);continue;}
        if(Now()>=job->retryDeadline){job->retryExpired=true;SetState(job,State::Failed);job->retryDeadline=0;std::string ignored;BatchCommand(item.peer,"/api/batch-status",job->batchId,job->webUpload+"\t5\t"+Hex(Utf8(L"传输失败超时（自动重试已达 30 秒）"))+"\n",ignored,{});continue;}
        std::string response;int status=BatchCommand(item.peer,"/api/retry-upload/"+job->webUpload,job->batchId,"",response,job);
        if(status==200&&response=="7"){job->paused=true;job->remotePaused=true;job->retryDeadline=Now()+30000;}
        else if(status==200&&response=="3"){Progress(job,job->view.total);SetState(job,State::Completed);}
        else if(status==200&&response=="8"){job->remotePaused=false;job->paused=false;job->retryDeadline=Now()+30000;job->view.activity=L"等待浏览器继续保存";}
        else if(status==410){job->cancel=true;SetState(job,State::Cancelled,L"接收端已取消本次传输");}
        else if(status==200){if(job->remotePaused){job->remotePaused=false;job->paused=false;job->retryDeadline=Now()+30000;}SendOne(item.peer,item.path,job->view.name,job);}
        bool retry=false;{std::lock_guard lock(mutex_);if(job->view.state!=State::Completed&&!job->cancel){if(job->retryExpired){job->view.state=State::Failed;job->view.error=L"传输失败超时（自动重试已达 30 秒）";job->view.activity.clear();}else if(!PermanentTransferFailure(job->view.error)){job->view.state=State::Waiting;job->view.activity=L"独立通道自动重试中";TraceJobLocked(job);retry=true;}}if(!retry){job->retryDeadline=0;job->view.activity.clear();}}
        if(retry){std::unique_lock lock(mutex_);retryCv_.wait_for(lock,std::chrono::milliseconds(400));retryQueue_.push_back(item);}
        else if(job->retryExpired||PermanentTransferFailure(job->view.error)){std::string ignored;BatchCommand(item.peer,"/api/batch-status",job->batchId,job->webUpload+"\t5\t"+Hex(Utf8(job->view.error))+"\n",ignored,{});}
    }
}
bool Engine::RetryTransfer(uint64_t id, std::wstring &error) {
    std::shared_ptr<Job> retry;Peer retryPeer;fs::path retryPath;
    {std::lock_guard lock(mutex_);for(auto &previous:jobs_)if(previous->view.id==id&&previous->view.state==State::Failed&&!previous->view.receiving&&previous->view.canRetry&&!previous->retryDeadline){
        const auto peer=std::find_if(peers_.begin(),peers_.end(),[&](const auto &p){return p.id==previous->targetId;});
        if(peer==peers_.end()){error=L"原目标设备已离线";return false;}
        retryPeer=*peer;retryPath=previous->archiveFile?previous->archiveFile->path:previous->view.savedPath;
        retry=std::make_shared<Job>();retry->view=previous->view;retry->view.id=nextJob_++;retry->view.state=State::Waiting;retry->view.done=0;retry->view.error.clear();retry->view.removed=false;retry->view.folderStopped=false;
        retry->batchId=previous->batchId;retry->batchItem=previous->batchItem;retry->webUpload=previous->webUpload;retry->folderId=previous->folderId;retry->targetId=previous->targetId;retry->senderName=previous->senderName;retry->archiveSource=previous->archiveSource;retry->archiveFile=previous->archiveFile;retry->archivePlan=previous->archivePlan;retry->fileIdentity=previous->fileIdentity;retry->hasIdentity=previous->hasIdentity;previous->hidden=true;break;
    }if(retry)jobs_.push_back(retry);}
    if(retry){QueueRetry(retryPeer,retryPath,retry);return true;}
    Peer target; fs::path source;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto &job) { return job->view.id == id; });
        if (found == jobs_.end() || !(*found)->view.canRetry || (*found)->view.receiving || (*found)->view.state < State::Cancelled) {
            error = L"此记录没有可重试的发送文件"; return false;
        }
        const auto &job = **found;
        const auto peer = std::find_if(peers_.begin(), peers_.end(), [&](const auto &item) { return item.id == job.targetId; });
        if (peer == peers_.end()) { error = L"原目标设备已离线，请在设备重新上线后重试"; return false; }
        target = *peer; source = job.archiveSource.empty()?job.view.savedPath:job.archiveSource;
        for (const auto &other : jobs_) if (other->view.state < State::Completed && other->targetId == job.targetId && (other->archiveSource.empty()?other->view.savedPath:other->archiveSource) == source) {
            error = L"此文件已有进行中的发送任务"; return false;
        }
        if (job.archiveSource.empty() && !job.view.folderId.empty()) {
            const fs::path relative(job.view.name);
            for (auto part = ++relative.begin(); part != relative.end(); ++part) source = source.parent_path();
        }
    }
    std::error_code ec;
    if (source.empty() || !fs::exists(source, ec) || ec) { error = L"原文件已移动或删除，请重新选择文件"; return false; }
    SendFiles(target, {source}); return true;
}
void Engine::Pause(uint64_t id,bool paused){
    std::vector<std::shared_ptr<Job>> changed;
    {std::lock_guard lock(mutex_);auto selected=std::find_if(jobs_.begin(),jobs_.end(),[&](const auto &j){return j->view.id==id;});if(selected==jobs_.end())return;const auto first=*selected;
     for(const auto &job:jobs_)if(job==first||(!first->view.folderId.empty()&&job->view.folderId==first->view.folderId&&job->batchId==first->batchId&&job->view.receiving==first->view.receiving)){
       if(job->cancel||job->view.state==State::Completed||job->view.state==State::Rejected)continue;job->paused=paused;job->remotePaused=false;job->view.paused=paused;job->view.speed=0;if(!paused&&!job->recipient.empty()&&(job->hasIdentity||job->archivePlan))job->view.state=State::Waiting;if(!paused){job->retryExpired=false;if(job->retryDeadline)job->retryDeadline=Now()+30000;}job->cv.notify_all();changed.push_back(job);TraceJobLocked(job);}}
    if(paused){std::lock_guard lock(socketsMutex_);for(auto &[socket,weak]:sockets_)if(std::find(changed.begin(),changed.end(),weak.lock())!=changed.end())shutdown(socket,SD_BOTH);}
    for(const auto &job:changed)if(!job->view.receiving&&!job->targetId.empty())Launch([this,job,paused]{Peer target;bool found=false;{std::lock_guard lock(mutex_);for(const auto &peer:peers_)if(peer.id==job->targetId){target=peer;found=true;break;}}if(!found||target.browser)return;
      Config cfg;{std::lock_guard lock(mutex_);cfg=config_;}SOCKET socket=Connect(target.gateway.empty()?target.ip:target.gateway,target.port);if(socket==INVALID_SOCKET)return;TrackSocket(socket);std::string query="POST /api/pause-upload/"+job->webUpload+" HTTP/1.1\r\nHost: "+target.ip+":"+std::to_string(target.port)+"\r\nConnection: close\r\nX-Chuan-Id: "+Hex(Utf8(cfg.id))+"\r\nX-Chuan-Server: "+Utf8(target.owner.empty()?target.id:target.owner)+"\r\nX-Chuan-Batch: "+job->batchId+"\r\nX-Chuan-Paused: "+(paused?"1":"0")+"\r\nContent-Length: 0\r\n\r\n";SendAll(socket,query);ReleaseSocket(socket);});
    retryCv_.notify_all();
}
void Engine::Cancel(uint64_t id) {
    std::set<std::shared_ptr<Job>> cancelled;
    {
        std::lock_guard lock(mutex_);
        for (auto &j : jobs_)
            if (j->view.id == id) {
                cancelled.insert(j);
                break;
            }
        if (cancelled.empty())
            return;
        const auto selected = *cancelled.begin();
        if (!selected->view.folderId.empty()) for (auto &job : jobs_)
            if (job->view.folderId == selected->view.folderId && job->batchId == selected->batchId &&
                job->webClient == selected->webClient && job->recipient == selected->recipient &&
                job->view.receiving == selected->view.receiving && job->view.peer == selected->view.peer)
                cancelled.insert(job);
        for (auto it=cancelled.begin();it!=cancelled.end();) {
            const auto &job=*it;
            if(!job->view.folderId.empty())job->view.folderStopped=true;
            if (job->view.state>=State::Completed&&!job->paused&&!job->retryDeadline) { it=cancelled.erase(it);continue; }
            job->cancel=true;job->paused=false;job->remotePaused=false;job->view.paused=false;job->announced=false;job->view.awaitingApproval=false;job->view.state=State::Cancelled;job->view.speed=0;
            job->view.error=job->view.receiving ? L"接收端已取消本次传输" : L"发送已取消";
            job->finishedAt=Now();job->cv.notify_all();
            const auto owner=job->webClient.empty()?job->nativeOwner:job->webClient;
            if (!owner.empty() && !job->batchId.empty()) for (auto &[key,batch]:batchApprovals_) {
                if (key.ends_with(":"+owner+":"+job->batchId) && job->batchItem<batch.items.size())
                    batch.cancelledItems.insert(job->batchItem);
            }
            ++it;
        }
    }
    {
        std::lock_guard lock(socketsMutex_);
        for (auto &[s, j] : sockets_)
            if (cancelled.contains(j.lock()))
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
            if (!j->asyncOfferKey.empty()) CompleteOfferLocked(j, j->selection);
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
        return j->paused || j->retryDeadline || j->view.state == State::Queued || j->view.state == State::Waiting ||
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
    std::set<std::string> active;
    for(const auto &job:jobs_)if(job->paused||job->view.state<State::Completed||job->retryDeadline)active.insert(job->batchId);
    // History visibility must not remove protocol state or sources used by workers.
    for(auto &job:jobs_)if(job->view.state>=State::Completed&&job->asyncOfferKey.empty()&&
        !active.contains(job->batchId)&&!busy.contains(TransferGroup(job->view)))job->hidden=true;
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
        return j->paused || j->view.state == State::Queued || j->view.state == State::Waiting ||
               j->view.state == State::Running;
    };
    if (active(job)) {
        error = L"请先取消传输，再删除记录";
        return false;
    }
    const auto path = job->view.savedPath;
    // Directory markers and matched pre-existing files are not owned by this copy.
    if (removeFile && !job->view.directory && !job->view.skipped && !path.empty()) {
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
                          "\r\nX-Chuan-Platform: windows\r\nX-Chuan-Port: " + std::to_string(boundPort_) + "\r\n\r\n",
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
        const auto platform=field("platform");peer.platform=platform=="windows"||platform=="android"?platform:"";
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
