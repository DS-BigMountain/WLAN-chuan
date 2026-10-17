#include "core.h"
#include <aclapi.h>
#include <fstream>
#include <iostream>
#include <sddl.h>
#include <shlobj.h>
#include <stdexcept>
using namespace chuan;
namespace {
unsigned checks = 0;
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
bool Restricted() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("query process token");
    DWORD container = 0, bytes = 0;
    GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &bytes);
    const bool restricted = IsTokenRestricted(token) || container != 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    DWORD integrity = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), bytes, &bytes)) {
        const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer.data())->Label.Sid;
        integrity = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    }
    CloseHandle(token);
    return restricted || (integrity != 0 && integrity < SECURITY_MANDATORY_MEDIUM_RID);
}
unsigned short FreePort(int type) {
    const auto socketValue = socket(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int bound = bind(socketValue, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    int length = sizeof(address);
    const int found = getsockname(socketValue, reinterpret_cast<sockaddr *>(&address), &length);
    closesocket(socketValue);
    if (bound || found) throw std::runtime_error("allocate test port");
    return ntohs(address.sin_port);
}
Config Fixture(const fs::path &directory) {
    auto config = LoadConfig(directory);
    config.port = FreePort(SOCK_STREAM);
    config.discoveryPort = FreePort(SOCK_DGRAM);
    config.receiveDir = directory / L"received";
    return config;
}
void CopySettings(const fs::path &source, const fs::path &destination) {
    fs::create_directories(destination);
    fs::copy_file(source / L"settings.ini", destination / L"settings.ini");
}
void SetIntegrity(const fs::path &file, bool low) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(low ? L"S:(ML;;NW;;;LW)" : L"S:(ML;;NW;;;ME)",
                                                               SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("prepare test integrity label");
    PACL sacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorSacl(descriptor, &present, &sacl, &defaulted);
    const auto result = SetNamedSecurityInfoW(const_cast<wchar_t *>(file.c_str()), SE_FILE_OBJECT,
                                              LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl);
    LocalFree(descriptor);
    Check(result == ERROR_SUCCESS, "set the copied test executable integrity label");
}
void CheckDefaultSelection() {
    const auto portableDir = ExePath().parent_path() / L"data";
    const bool automatic = GetPrivateProfileIntW(L"Chuan", L"AutomaticStorageFallback", 0,
                                                 (portableDir / L"settings.ini").c_str()) != 0;
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
        throw std::runtime_error("resolve user configuration directory");
    const auto userDir = fs::path(path) / L"Chuan";
    CoTaskMemFree(path);
    const auto config = LoadConfig();
    const auto expected = automatic && !Restricted() ? userDir : portableDir;
    if (config.dataDir != expected)
        std::cerr << "selection: actual=" << Utf8(config.dataDir.wstring()) << ", expected=" << Utf8(expected.wstring())
                  << ", automatic=" << automatic << ", restricted=" << Restricted() << '\n';
    Check(config.dataDir == expected, "default storage follows portable intent and process restrictions");
    if (expected == portableDir)
        Check(config.receiveDir == portableDir / L"received", "portable receive path resolves beside this executable");
}
void RunChild(const fs::path &directory, const wchar_t *mode) {
    fs::create_directories(directory);
    const auto executable = directory / L"storage_tests.exe";
    fs::copy_file(ExePath(), executable);
    if (!Restricted())
        SetIntegrity(executable, std::wstring(mode) == L"--check-restricted-startup");
    auto command = L"\"" + executable.wstring() + L"\" " + mode;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
        throw std::runtime_error("start profile selection test");
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(process.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0 || code != 0)
        throw std::runtime_error("profile selection child exit=" + std::to_string(code) + ", wait=" + std::to_string(waited));
    Check(true, "profile selection child succeeds");
}
void RunSelectionChild(const fs::path &directory, bool automatic) {
    auto config = Fixture(directory / L"data");
    config.automaticStorageFallback = automatic;
    SaveConfig(config);
    RunChild(directory, L"--check-default-selection");
}
void CheckRestrictedStartup() {
    Check(Restricted(), "restricted startup fixture requires a restricted process");
    auto config = LoadConfig();
    config.port = FreePort(SOCK_STREAM);
    config.discoveryPort = FreePort(SOCK_DGRAM);
    Engine first(config);
    std::wstring error;
    Check(first.Start(error), "restricted startup recovers from denied user directories");
    const auto state = first.GetSnapshot();
    Check(state.config.automaticStorageFallback && state.config.dataDir == ExePath().parent_path() / L"data" &&
          (state.notice.find(L"受限访问令牌") != std::wstring::npos || state.notice.find(L"低完整性级别") != std::wstring::npos),
          "access denied diagnosis identifies process restrictions and automatic fallback");
    first.Stop();
    config = LoadConfig();
    config.port = FreePort(SOCK_STREAM);
    config.discoveryPort = FreePort(SOCK_DGRAM);
    Engine second(config);
    Check(second.Start(error) && second.GetSnapshot().notice.empty(),
          "the next restricted startup reads its saved fallback without a repeated warning");
    second.Stop();
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--check-default-selection") {
            CheckDefaultSelection();
        } else if (argc == 2 && std::wstring(argv[1]) == L"--check-restricted-startup") {
            CheckRestrictedStartup();
        } else {
            const auto work = fs::current_path() / (L"storage-scenarios-" + std::to_wstring(GetCurrentProcessId()) +
                                                   L"-" + std::to_wstring(GetTickCount64()));
            fs::create_directories(work);
            {
                const auto original = Fixture(work / L"original" / L"data");
                SaveConfig(original);
                Check(LoadConfig(original.dataDir).receiveDir == original.receiveDir, "existing configuration round trip");
                fs::create_directories(original.receiveDir);
                std::ofstream(original.receiveDir / L"keep.txt") << "preserved";
                const auto moved = work / L"moved" / L"data";
                CopySettings(original.dataDir, moved);
                auto config = LoadConfig(moved);
                Check(config.receiveDir == moved / L"received" && config.id == original.id,
                      "copied portable configuration rebases its internal receive path");
                config.port = FreePort(SOCK_STREAM);
                config.discoveryPort = FreePort(SOCK_DGRAM);
                Engine engine(config);
                std::wstring error;
                Check(engine.Start(error) && engine.GetSnapshot().notice.empty(), "moved configuration starts without a storage warning");
                engine.Stop();
                Check(fs::exists(original.receiveDir / L"keep.txt") && fs::is_directory(moved / L"received"),
                      "portable relocation preserves the old files and creates the new receive directory");
                Check(GetPrivateProfileIntW(L"Chuan", L"AutomaticStorageFallback", 2,
                      (moved / L"settings.ini").c_str()) == 0, "explicit storage remains distinct from automatic fallback");
                const auto legacy = work / L"legacy" / L"data";
                CopySettings(original.dataDir, legacy);
                Check(WritePrivateProfileStringW(L"Chuan", L"SavedDataDir", nullptr, (legacy / L"settings.ini").c_str()),
                      "prepare legacy portable configuration");
                Check(LoadConfig(legacy).receiveDir == legacy / L"received", "legacy default portable path rebases after copying");
            }
            {
                auto original = Fixture(work / L"nested-original");
                original.receiveDir = original.dataDir / L"inbox" / L"资料";
                SaveConfig(original);
                const auto moved = work / L"nested-moved";
                CopySettings(original.dataDir, moved);
                Check(LoadConfig(moved).receiveDir == moved / L"inbox" / L"资料", "nested internal receive paths rebase");
                original.receiveDir = work / L"external-received";
                SaveConfig(original);
                const auto custom = work / L"custom-moved";
                CopySettings(original.dataDir, custom);
                Check(LoadConfig(custom).receiveDir == original.receiveDir, "custom external receive paths retain their absolute location");
                Check(WritePrivateProfileStringW(L"Chuan", L"ReceiveDir", L"received", (custom / L"settings.ini").c_str()),
                      "prepare relative receive path");
                Check(LoadConfig(custom).receiveDir == custom / L"received", "relative receive paths use the configuration directory");
                const auto foreign = custom / L"settings.ini";
                Check(WritePrivateProfileStringW(L"Chuan", L"SavedDataDir", L"Z:\\旧电脑\\Chuan\\data", foreign.c_str()) &&
                      WritePrivateProfileStringW(L"Chuan", L"ReceiveDir", L"Z:\\旧电脑\\Chuan\\data\\received\\资料", foreign.c_str()),
                      "prepare a configuration from another computer");
                Check(LoadConfig(custom).receiveDir == custom / L"received" / L"资料",
                      "portable paths from another computer use the current configuration root");
            }
            {
                auto config = Fixture(work / L"blocked-config");
                std::ofstream(config.dataDir) << "preserved";
                config.receiveDir = work / L"writable-received";
                config.fallbackDataDir = work / L"config-fallback" / L"data";
                Engine engine(config);
                std::wstring error;
                Check(engine.Start(error), "configuration-only failure recovers to portable storage");
                const auto state = engine.GetSnapshot();
                Check(state.config.receiveDir == config.receiveDir && state.config.dataDir == config.fallbackDataDir &&
                      state.config.automaticStorageFallback && state.notice.find(config.dataDir.wstring()) != std::wstring::npos,
                      "configuration recovery preserves the working receive directory and reports the original path");
                Check(LoadConfig(config.fallbackDataDir).automaticStorageFallback && fs::is_regular_file(config.dataDir),
                      "automatic configuration fallback persists without replacing the blocked file");
                engine.Stop();
            }
            {
                auto config = Fixture(work / L"receive-config");
                config.receiveDir = work / L"blocked-received";
                std::ofstream(config.receiveDir) << "preserved";
                std::ofstream(config.dataDir) << "preserved";
                Engine failed(config);
                std::wstring error;
                Check(!failed.Start(error) && error.find(config.receiveDir.wstring()) != std::wstring::npos,
                      "unavailable storage fails with the original path");
                config.fallbackDataDir = work / L"receive-fallback" / L"data";
                Engine recovered(config);
                Check(recovered.Start(error), "unavailable receive and user storage recover to portable storage");
                const auto state = recovered.GetSnapshot();
                Check(state.config.receiveDir == config.fallbackDataDir / L"received" && state.config.automaticStorageFallback &&
                      LoadConfig(config.fallbackDataDir).id == config.id,
                      "receive fallback preserves identity and records its automatic origin");
                recovered.Stop();
            }
            {
                auto config = Fixture(work / L"readonly-config");
                SaveConfig(config);
                const auto ini = config.dataDir / L"settings.ini";
                Check(SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_READONLY), "prepare readonly settings");
                Engine engine(config);
                std::wstring error;
                const bool started = engine.Start(error);
                auto changed = config;
                changed.name = L"unsaved name";
                const bool updated = engine.UpdateConfig(changed, error);
                SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
                Check(started && !updated && engine.GetSnapshot().config.name == config.name &&
                      LoadConfig(config.dataDir).name == config.name, "readonly explicit settings report failure without changing active settings");
                engine.Stop();
            }
            RunSelectionChild(work / L"automatic-selection", true);
            RunSelectionChild(work / L"intentional-selection", false);
            RunChild(work / L"restricted-startup", L"--check-restricted-startup");
        }
        std::cout << "storage checks passed: " << checks << ", restricted_token=" << Restricted() << '\n';
        WSACleanup();
        CoUninitialize();
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        WSACleanup();
        CoUninitialize();
        return 1;
    }
}
