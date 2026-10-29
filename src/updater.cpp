#include "updater.h"
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <winhttp.h>
#include <winver.h>

namespace chuan::update {
namespace {
struct Internet {
    HINTERNET value = nullptr;
    explicit Internet(HINTERNET h) : value(h) { if (!h) throw std::runtime_error("无法连接 GitHub，请检查网络后重试"); }
    ~Internet() { WinHttpCloseHandle(value); }
    operator HINTERNET() const { return value; }
};
void Require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::wstring Quote(const std::wstring &value) {
    std::wstring out = L"\""; size_t slashes = 0;
    for (auto c : value) { if (c == L'\\') { ++slashes; continue; } if (c == L'"') out.append(slashes * 2 + 1, L'\\'); else out.append(slashes, L'\\'); slashes = 0; out += c; }
    out.append(slashes * 2, L'\\'); return out + L'"';
}
HANDLE Start(const fs::path &exe, const std::wstring &args, bool hidden = true) {
    auto command = Quote(exe.wstring()) + L" " + args;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (hidden) { startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE; }
    Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0,
                           nullptr, exe.parent_path().c_str(), &startup, &process) != FALSE, "无法启动更新进程");
    CloseHandle(process.hThread); return process.hProcess;
}
HANDLE StartUnelevated(const fs::path &exe, const std::wstring &args) {
    HANDLE current = nullptr; TOKEN_ELEVATION elevation{}; DWORD size = sizeof(elevation);
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current) != FALSE, "无法检查进程权限");
    GetTokenInformation(current, TokenElevation, &elevation, sizeof(elevation), &size); CloseHandle(current);
    if (!elevation.TokenIsElevated) return Start(exe, args);
    DWORD pid = 0; GetWindowThreadProcessId(GetShellWindow(), &pid);
    HANDLE shell = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), token = nullptr;
    if (!shell) throw std::runtime_error("无法获取普通用户会话，请以普通权限运行后更新");
    const bool opened = OpenProcessToken(shell, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &token) != FALSE; CloseHandle(shell);
    Require(opened, "无法获取普通用户权限，请以普通权限运行后更新");
    auto command = Quote(exe.wstring()) + L" " + args; STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    const bool started = CreateProcessWithTokenW(token, LOGON_WITH_PROFILE, exe.c_str(), command.data(), CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &startup, &process) != FALSE;
    CloseHandle(token); Require(started, "无法以普通用户权限启动更新进程"); CloseHandle(process.hThread); return process.hProcess;
}
DWORD WaitProcess(HANDLE process, DWORD timeout) {
    const auto wait = WaitForSingleObject(process, timeout); DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process, &code);
    else TerminateProcess(process, 1);
    CloseHandle(process); Require(wait == WAIT_OBJECT_0, "更新处理超时"); return code;
}
std::wstring Header(HINTERNET request, DWORD header) {
    DWORD size = 0; WinHttpQueryHeaders(request, header, nullptr, nullptr, &size, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
    std::wstring value(size / sizeof(wchar_t), 0);
    Require(WinHttpQueryHeaders(request, header, nullptr, value.data(), &size, nullptr) != FALSE, "无法读取下载响应");
    value.resize(size / sizeof(wchar_t)); return value;
}
void Request(std::wstring url, uint64_t limit, std::atomic<bool> &cancel,
             const std::function<void(const char *, size_t)> &consume) {
    Internet session(WinHttpOpen(L"Chuan-Updater/2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session, 10000, 15000, 15000, 15000);
    for (int redirect = 0; redirect < 6; ++redirect) {
        if (cancel) throw std::runtime_error("更新操作已取消");
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts); parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        Require(WinHttpCrackUrl(url.c_str(), 0, 0, &parts) != FALSE && parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort == 443, "更新下载地址必须使用 HTTPS");
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        Require(host == L"api.github.com" || host == L"github.com" || host == L"release-assets.githubusercontent.com" || host == L"objects.githubusercontent.com", "更新下载重定向地址无效");
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength); if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        Internet connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
        Internet request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        const wchar_t *headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (host != L"api.github.com") headers = L"Accept: application/octet-stream\r\n";
        Require(WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE && WinHttpReceiveResponse(request, nullptr) != FALSE,
                "GitHub 连接失败或超时，请检查网络后重试");
        DWORD status = 0, bytes = sizeof(status); WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &bytes, nullptr);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) { url = Header(request, WINHTTP_QUERY_LOCATION); continue; }
        Require(status != 403 && status != 429, "GitHub 请求受限，请稍后重试或打开发布页");
        Require(status == 200, "GitHub 未返回可用更新资源，请稍后重试");
        const auto length = Header(request, WINHTTP_QUERY_CONTENT_LENGTH);
        if (!length.empty()) Require(std::stoull(length) <= limit, "更新响应超出大小限制");
        std::array<char, 128 * 1024> buffer{}; uint64_t total = 0;
        for (;;) { if (cancel) throw std::runtime_error("更新操作已取消"); DWORD count = 0; Require(WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &count) != FALSE, "更新下载中断，请重试"); if (!count) break; total += count; Require(total <= limit, "更新响应超出大小限制"); consume(buffer.data(), count); }
        return;
    }
    throw std::runtime_error("更新下载重定向次数过多");
}
std::wstring PsLiteral(std::wstring value) { size_t at = 0; while ((at = value.find(L'\'', at)) != std::wstring::npos) { value.insert(at, 1, L'\''); at += 2; } return L"'" + value + L"'"; }
std::wstring Base64(const std::wstring &value) {
    static constexpr wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto *data = reinterpret_cast<const unsigned char *>(value.data()); const size_t count = value.size() * sizeof(wchar_t); std::wstring out;
    for (size_t i = 0; i < count; i += 3) { unsigned n = static_cast<unsigned>(data[i]) << 16; if (i + 1 < count) n |= static_cast<unsigned>(data[i + 1]) << 8; if (i + 2 < count) n |= data[i + 2]; out += alphabet[n >> 18]; out += alphabet[(n >> 12) & 63]; out += i + 1 < count ? alphabet[(n >> 6) & 63] : L'='; out += i + 2 < count ? alphabet[n & 63] : L'='; } return out;
}
void Extract(const fs::path &zip, const fs::path &exe, const std::string &version) {
    // Only the root executable is extracted. Archive paths never become filesystem destinations.
    const auto script = L"$ErrorActionPreference='Stop';try{Add-Type -AssemblyName System.IO.Compression.FileSystem;"
        L"$z=[IO.Compression.ZipFile]::OpenRead(" + PsLiteral(zip.wstring()) + L");try{"
        L"$e=@($z.Entries|Where-Object{$_.FullName -ceq 'Chuan.exe'});"
        L"if($e.Count -ne 1 -or $e[0].Length -lt 4096 -or $e[0].Length -gt 268435456){throw 'Invalid executable'};"
        L"[IO.Compression.ZipFileExtensions]::ExtractToFile($e[0]," + PsLiteral(exe.wstring()) + L",$false);"
        L"$a=@($z.Entries|Where-Object{$_.FullName -ceq " + PsLiteral(L"Chuan-" + Wide(version.substr(1)) + L"-android.apk") + L"});"
        L"if($a.Count -gt 1){throw 'Duplicate APK'};if($a.Count -eq 1){if($a[0].Length -lt 1 -or $a[0].Length -gt 536870912){throw 'Invalid APK'};"
        L"[IO.Compression.ZipFileExtensions]::ExtractToFile($a[0]," + PsLiteral((exe.parent_path() / (L"Chuan-" + Wide(version.substr(1)) + L"-android.apk")).wstring()) + L",$false)}"
        L"}finally{$z.Dispose()};exit 0}catch{exit 1}";
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    Require(WaitProcess(Start(fs::path(system) / L"WindowsPowerShell/v1.0/powershell.exe", L"-NoLogo -NoProfile -NonInteractive -EncodedCommand " + Base64(script)), 120000) == 0, "更新包解压失败");
}
std::wstring Unique() { GUID guid{}; Require(SUCCEEDED(CoCreateGuid(&guid)), "无法创建更新目录"); wchar_t text[40]{}; StringFromGUID2(guid, text, 40); return text; }
fs::path NewStage() {
    PWSTR raw = nullptr; Require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)), "无法访问更新缓存目录");
    fs::path root(raw); CoTaskMemFree(raw); root /= L"Chuan/Updates"; fs::create_directories(root);
    auto stage = root / Unique(); Require(fs::create_directory(stage), "无法创建更新缓存目录"); return stage;
}
struct Plan { fs::path source, target, data; std::string digest, version; std::wstring transaction; std::string apkDigest; };
void SavePlan(const fs::path &path, const Plan &p) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << std::quoted(Utf8(p.source.wstring())) << '\n' << std::quoted(Utf8(p.target.wstring())) << '\n'
        << std::quoted(Utf8(p.data.wstring())) << '\n' << std::quoted(p.digest) << '\n' << std::quoted(p.version) << '\n' << std::quoted(Utf8(p.transaction)) << '\n' << std::quoted(p.apkDigest);
    out.close(); Require(!out.fail(), "无法保存更新计划");
}
Plan LoadPlan(const fs::path &path) {
    Require(path.is_absolute() && path.filename() == L"plan" && path.parent_path() == ExePath().parent_path() && fs::file_size(path) < 65536, "更新计划路径无效");
    std::ifstream in(path, std::ios::binary); std::string source, target, data, digest, version, transaction, apkDigest;
    in >> std::quoted(source) >> std::quoted(target) >> std::quoted(data) >> std::quoted(digest) >> std::quoted(version) >> std::quoted(transaction) >> std::quoted(apkDigest);
    Require(!in.fail(), "更新计划无效"); Plan p{Wide(source), Wide(target), Wide(data), digest, version, Wide(transaction), apkDigest};
    Require(p.apkDigest.empty() || (p.apkDigest.size() == 64 && p.apkDigest.find_first_not_of("0123456789abcdef") == std::string::npos), "附带安装包摘要无效");
    Require(p.source == path.parent_path() / L"Chuan.exe" && p.target.is_absolute() && p.target.extension() == L".exe" && p.target.parent_path() != path.parent_path() && p.data.is_absolute(), "更新目标路径无效");
    Require(p.transaction.size() == 38 && p.transaction.front() == L'{' && p.transaction.back() == L'}' && p.transaction.find_first_not_of(L"{}0123456789abcdefABCDEF-") == std::wstring::npos, "更新事务标识无效");
    ParseVersion(p.version); return p;
}
bool Elevated() { HANDLE token = nullptr; TOKEN_ELEVATION value{}; DWORD size = sizeof(value); if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false; GetTokenInformation(token, TokenElevation, &value, sizeof(value), &size); CloseHandle(token); return value.TokenIsElevated != 0; }
HANDLE RestartNormal(const Plan &p, const std::wstring &event = {}) {
    // The coordinator retains the original token, including when only replacement needed UAC.
    return Start(p.target, L"--data-dir " + Quote(p.data.wstring()) + (event.empty() ? L"" : L" --update-event " + Quote(event)), false);
}
void Restore(const Plan &p) {
    const auto backup = p.target.parent_path() / (L".Chuan-backup-" + p.transaction + L".exe");
    Require(fs::exists(backup), "恢复备份不存在，请保留当前程序目录"); VerifyExecutable(backup, Utf8(Version));
    if (!MoveFileExW(backup.c_str(), p.target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "无法恢复旧版程序");
}
void Apply(const Plan &p) {
    if (!p.apkDigest.empty()) {
        const auto name = L"Chuan-" + Wide(p.version.substr(1)) + L"-android.apk";
        auto source = p.source.parent_path() / name, target = p.target.parent_path() / name;
        Require(FileHash(source) == p.apkDigest, "附带 Android 安装包校验失败");
        if (!fs::exists(target) && !CopyFileW(source.c_str(), target.c_str(), TRUE))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "无法更新附带 Android 安装包");
        Require(FileHash(target) == p.apkDigest, "程序目录已有同名且内容不同的 Android 安装包");
    }
    ReplaceExecutable(p.source, p.target, p.digest, p.version, p.transaction);
}
void Execute(const Plan &plan, const fs::path &planPath, bool rollback) {
    try { if (rollback) Restore(plan); else Apply(plan); }
    catch (const std::system_error &e) {
        if (e.code().value() != ERROR_ACCESS_DENIED && e.code().value() != ERROR_PRIVILEGE_NOT_HELD) throw;
        SHELLEXECUTEINFOW launch{sizeof(launch)}; launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        auto args = std::wstring(rollback ? L"--rollback-update " : L"--replace-update ") + Quote(planPath.wstring()); auto self = ExePath().wstring();
        launch.lpVerb = L"runas"; launch.lpFile = self.c_str(); launch.lpParameters = args.c_str(); launch.nShow = SW_HIDE;
        Require(ShellExecuteExW(&launch) != FALSE, "更新授权已取消，保留当前版本");
        Require(WaitProcess(launch.hProcess, 120000) == 0, "更新文件处理失败，请保留目录中的备份程序");
    }
}
} // namespace

std::string FileHash(const fs::path &path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "无法初始化更新校验");
    struct Cleanup { BCRYPT_ALG_HANDLE &a; BCRYPT_HASH_HANDLE &h; ~Cleanup() { if (h) BCryptDestroyHash(h); if (a) BCryptCloseAlgorithmProvider(a, 0); } } cleanup{algorithm, hash};
    Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "无法初始化更新校验");
    std::ifstream in(path, std::ios::binary); Require(in.is_open(), "无法读取更新文件"); std::array<char, 128 * 1024> buffer{};
    while (in) { in.read(buffer.data(), buffer.size()); auto count = in.gcount(); if (count) Require(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) >= 0, "更新文件校验失败"); }
    Require(in.eof(), "无法完整读取更新文件"); std::array<unsigned char, 32> digest{}; Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "更新文件校验失败");
    std::ostringstream out; for (auto c : digest) out << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(c); return out.str();
}
void VerifyExecutable(const fs::path &path, const std::string &version) {
    const auto wanted = ParseVersion(version); DWORD ignored = 0, size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    Require(size != 0 && size < 1048576, "更新程序缺少版本信息"); std::vector<char> data(size);
    Require(GetFileVersionInfoW(path.c_str(), 0, size, data.data()) != FALSE, "无法读取更新程序版本");
    VS_FIXEDFILEINFO *info = nullptr; UINT length = 0;
    Require(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) != FALSE && length >= sizeof(*info) && info->dwSignature == 0xFEEF04BD,
            "更新程序版本信息无效");
    Require(HIWORD(info->dwProductVersionMS) == wanted[0] && LOWORD(info->dwProductVersionMS) == wanted[1] && HIWORD(info->dwProductVersionLS) == wanted[2], "更新程序版本与发行版本不一致");
    wchar_t *name = nullptr;
    Require(VerQueryValueW(data.data(), L"\\StringFileInfo\\080404b0\\OriginalFilename", reinterpret_cast<void **>(&name), &length) && name && std::wstring(name) == L"Chuan.exe", "更新包不是传客户端");
    std::ifstream in(path, std::ios::binary); IMAGE_DOS_HEADER dos{}; in.read(reinterpret_cast<char *>(&dos), sizeof(dos));
    Require(in.good() && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= sizeof(dos) && dos.e_lfanew < 1048576, "更新程序格式无效");
    in.seekg(dos.e_lfanew); DWORD signature = 0; IMAGE_FILE_HEADER header{}; in.read(reinterpret_cast<char *>(&signature), sizeof(signature)); in.read(reinterpret_cast<char *>(&header), sizeof(header));
    Require(in.good() && signature == IMAGE_NT_SIGNATURE && header.Machine == IMAGE_FILE_MACHINE_AMD64, "更新程序不适用于 Windows x64");
}
void FetchPackage(const Release &release, const fs::path &file, std::atomic<bool> &cancel,
                  const std::function<void(uint64_t)> &progress) {
    try {
        std::ofstream out(file, std::ios::binary); Require(out.is_open(), "无法创建更新缓存文件"); uint64_t done = 0;
        Request(Wide(release.url), release.size, cancel, [&](const char *data, size_t n) { out.write(data, n); Require(out.good(), "更新下载写入失败，请检查可用空间"); done += n; progress(done); });
        out.close(); Require(!out.fail() && done == release.size, "更新包下载不完整，请重试");
    } catch (...) { std::error_code ec; fs::remove(file, ec); throw; }
}
void PreparePackage(const Release &release, const fs::path &archive, const fs::path &stage) {
    Require(FileHash(archive) == release.digest, "更新包 SHA-256 校验失败，请重试");
    Extract(archive, stage / L"Chuan.exe", release.version); VerifyExecutable(stage / L"Chuan.exe", release.version);
}
void ReplaceExecutable(const fs::path &source, const fs::path &target, const std::string &digest,
                       const std::string &version, const std::wstring &transaction) {
    Require(FileHash(source) == digest, "更新程序校验失败，已保留旧版"); VerifyExecutable(source, version);
    VerifyExecutable(target, Utf8(Version));
    Require(source != target && target.is_absolute() && transaction.size() == 38 && transaction.find_first_not_of(L"{}0123456789abcdefABCDEF-") == std::wstring::npos, "更新目标或事务标识无效");
    const auto incoming = target.parent_path() / (L".Chuan-new-" + transaction + L".exe");
    const auto backup = target.parent_path() / (L".Chuan-backup-" + transaction + L".exe");
    Require(!fs::exists(incoming) && !fs::exists(backup), "更新事务文件已存在");
    if (!CopyFileW(source.c_str(), incoming.c_str(), TRUE)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "无法写入程序目录");
    try {
        Require(FileHash(incoming) == digest, "程序目录中的更新文件校验失败");
        // ReplaceFile atomically preserves the original under a unique backup path.
        if (!ReplaceFileW(target.c_str(), incoming.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
            DWORD error = GetLastError();
            if (!fs::exists(target) && fs::exists(backup)) MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
            throw std::system_error(static_cast<int>(error), std::system_category(), "替换程序失败");
        }
        try { Require(FileHash(target) == digest, "替换后的程序校验失败"); VerifyExecutable(target, version); }
        catch (...) { Require(MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE, "更新失败且自动恢复未完成，请保留目录中的备份程序"); throw; }
        // Retain the uniquely named old executable for recovery after startup.
    } catch (...) { std::error_code ec; fs::remove(incoming, ec); throw; }
}
Manager::~Manager() { Cancel(); Join(); std::error_code ec; if (!stage_.empty()) fs::remove_all(stage_, ec); }
void Manager::Join() { if (worker_.joinable()) worker_.join(); }
Status Manager::Get() const { std::lock_guard lock(mutex_); auto state = status_; state.settingsUnread = notices_.SettingsUnread(); state.updateUnread = notices_.UpdateUnread(); return state; }
void Manager::SettingsRead() { std::lock_guard lock(mutex_); notices_.SettingsRead(); }
void Manager::UpdateRead() { std::lock_guard lock(mutex_); notices_.UpdateRead(); }
void Manager::Set(Phase phase, const std::wstring &message) { std::lock_guard lock(mutex_); status_.phase = phase; status_.message = message; }
void Manager::Cancel() { cancel_ = true; }
void Manager::Check() {
    auto s = Get(); if (s.phase == Phase::Checking || s.phase == Phase::Downloading) return;
    Join(); cancel_ = false; Set(Phase::Checking, L"正在检查 GitHub 更新…");
    worker_ = std::thread([this] { try {
        std::string body; Request(Wide(ReleaseApi), 1048576, cancel_, [&](const char *data, size_t n) { body.append(data, n); });
        // Compare first so older releases without modern asset digests still report current correctly.
        const auto root = JsonReader(body).Read(); auto latest = root.At("tag_name").StringValue();
        Require(root.kind == JsonValue::Object && !root.At("draft").True() && !root.At("prerelease").True() && latest.starts_with("V"), "未找到有效的正式发行版本");
        if (ParseVersion(latest) <= ParseVersion(Utf8(Version))) { { std::lock_guard lock(mutex_); notices_.version.clear(); } Set(Phase::Current, L"当前已是最新正式版本 V" + std::wstring(Version)); return; }
        auto release = ParseRelease(body, "windows-x64.zip");
        { std::lock_guard lock(mutex_); release_ = release; status_.version = Wide(release.version); notices_.Found(status_.version); status_.notes = Wide(release.notes); status_.done = 0; status_.total = release.size; }
        Set(Phase::Available, L"发现新版本 " + Wide(release.version));
    } catch (const std::exception &e) { Set(Phase::Error, Wide(e.what())); } });
}
void Manager::Download() {
    if (Get().phase != Phase::Available) return; Join(); cancel_ = false; Set(Phase::Downloading, L"正在下载更新…");
    worker_ = std::thread([this] { try {
        std::error_code ec; if (!stage_.empty()) fs::remove_all(stage_, ec); stage_ = NewStage();
        const auto archive = stage_ / L"package.zip";
        FetchPackage(release_, archive, cancel_, [&](uint64_t done) { std::lock_guard lock(mutex_); status_.done = done; });
        Set(Phase::Downloading, L"正在校验更新包…"); Require(!cancel_, "更新操作已取消"); PreparePackage(release_, archive, stage_);
        Require(!cancel_, "更新操作已取消"); Set(Phase::Ready, L"下载完成，需要重启软件以应用更新。");
    } catch (const std::exception &e) { Set(Phase::Error, Wide(e.what())); } });
}
bool Manager::Restart(HWND, const fs::path &dataDir, std::wstring &error) {
    if (Get().phase != Phase::Ready) return false; Join();
    try {
        auto helper = stage_ / L"helper.exe"; fs::copy_file(ExePath(), helper, fs::copy_options::overwrite_existing);
        Plan plan{stage_ / L"Chuan.exe", ExePath(), dataDir, FileHash(stage_ / L"Chuan.exe"), release_.version, Unique(), {}};
        const auto apk = stage_ / (L"Chuan-" + Wide(release_.version.substr(1)) + L"-android.apk");
        if (fs::exists(apk)) plan.apkDigest = FileHash(apk);
        SavePlan(stage_ / L"plan", plan);
        const auto args = L"--apply-update " + Quote((stage_ / L"plan").wstring()) + L" --parent " + std::to_wstring(GetCurrentProcessId());
        CloseHandle(StartUnelevated(helper, args)); stage_.clear(); return true;
    } catch (const std::exception &e) { error = Wide(e.what()); return false; }
}
int RunHelper(const std::map<std::wstring, std::wstring> &options) {
    const bool apply = options.contains(L"--apply-update"), replace = options.contains(L"--replace-update"), rollback = options.contains(L"--rollback-update");
    if (!apply && !replace && !rollback) return -1;
    try {
        const fs::path planPath(options.at(apply ? L"--apply-update" : replace ? L"--replace-update" : L"--rollback-update")); const auto plan = LoadPlan(planPath);
        if (replace) { Apply(plan); return 0; }
        if (rollback) { Restore(plan); return 0; }
        Require(!Elevated(), "请以普通用户权限启动程序后执行在线更新");
        const auto pid = std::stoul(options.at(L"--parent")); HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (parent) { const auto wait = WaitForSingleObject(parent, 120000); CloseHandle(parent); Require(wait == WAIT_OBJECT_0, "主程序尚未退出，更新已取消"); }
        Execute(plan, planPath, false);
        const auto eventName = L"Local\\ChuanUpdate-" + plan.transaction;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!ready) { Execute(plan, planPath, true); throw std::runtime_error("无法确认新版启动，已恢复旧版"); }
        HANDLE child = nullptr;
        try {
            child = RestartNormal(plan, eventName);
            HANDLE waits[] = {ready, child}; const auto wait = WaitForMultipleObjects(2, waits, FALSE, 60000);
            if (wait != WAIT_OBJECT_0) {
                if (WaitForSingleObject(child, 0) != WAIT_OBJECT_0) { TerminateProcess(child, 1); WaitForSingleObject(child, 10000); }
                throw std::runtime_error("新版未能完成启动，已恢复旧版");
            }
        } catch (...) { if (child) CloseHandle(child); CloseHandle(ready); Execute(plan, planPath, true); throw; }
        CloseHandle(child); CloseHandle(ready);
        // Cleanup only the private staging directory; the backup remains recoverable.
        std::error_code ec; for (const auto *file : {L"package.zip", L"Chuan.exe", L"plan"}) fs::remove(planPath.parent_path() / file, ec);
        fs::remove(planPath.parent_path() / (L"Chuan-" + Wide(plan.version.substr(1)) + L"-android.apk"), ec);
        return 0;
    } catch (const std::exception &e) {
        if (apply) try { const auto p = LoadPlan(options.at(L"--apply-update")); CloseHandle(RestartNormal(p)); } catch (...) {}
        MessageBoxW(nullptr, Wide(e.what()).c_str(), L"在线更新未完成", MB_OK | MB_ICONERROR);
        return 1;
    }
}
} // namespace chuan::update
