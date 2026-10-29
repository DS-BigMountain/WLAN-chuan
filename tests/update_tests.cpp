#include "updater.h"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace chuan;
using namespace chuan::update;
namespace {
unsigned checks = 0;
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); ++checks; }
template<class F> void Reject(F action, const char *message) { try { action(); } catch (const std::exception &) { ++checks; return; } throw std::runtime_error(message); }
std::string Read(const fs::path &path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
void Change(std::string &s, const std::string &before, const std::string &after) { auto at = s.find(before); Check(at != std::string::npos, "fixture token"); s.replace(at, before.size(), after); }
}
int wmain(int argc, wchar_t **argv) {
    try {
        Check(argc >= 4, "usage: update_tests EXE RELEASE_JSON QA_DIR [--live]");
        const auto root = fs::absolute(argv[3]); fs::create_directories(root);
        const auto fixture = Read(argv[2]);
        auto desktop = ParseRelease(fixture, "windows-x64.zip"), android = ParseRelease(fixture, "android.apk");
        const auto fixtureVersion = JsonReader(fixture).Read().At("tag_name").StringValue();
        Check(desktop.version == fixtureVersion && desktop.size > 0 && desktop.digest.size() == 64, "Windows release");
        Check(android.asset == "Chuan-" + fixtureVersion.substr(1) + "-android.apk", "Android release");
        Check(ParseVersion("V2.10.0") > ParseVersion("V2.9.9"), "numeric version ordering");
        Check(ParseVersion("2.2.0") == ParseVersion("V2.2.0"), "current version format");
        Notices notices;
        Check(!notices.SettingsUnread() && !notices.UpdateUnread(), "initial update badges");
        notices.Found(L"V2.3.0");
        Check(notices.SettingsUnread() && notices.UpdateUnread(), "new version marks both entries");
        notices.SettingsRead();
        Check(!notices.SettingsUnread() && notices.UpdateUnread(), "settings clears only its own badge");
        notices.Found(L"V2.3.0");
        Check(!notices.SettingsUnread() && notices.UpdateUnread(), "same release must not relight settings");
        notices.UpdateRead();
        Check(!notices.SettingsUnread() && !notices.UpdateUnread(), "update details clear inner badge");
        notices.Found(L"V2.3.0");
        Check(!notices.SettingsUnread() && !notices.UpdateUnread(), "read release must remain read");
        notices.Found(L"V2.4.0");
        Check(notices.SettingsUnread() && notices.UpdateUnread(), "different release marks both entries again");
        for (auto bad : {"2.2", "V2.2.0.1", "V2.-1.0", "V65536.0.0", "V2.2.0-beta", "V2.2.0 ", "V2.2.0evil"}) Reject([&] { ParseVersion(bad); }, "invalid version accepted");
        Reject([&] { ParseRelease(fixture, "linux.zip"); }, "missing platform accepted");
        auto bad = fixture; Change(bad, "sha256:" + android.digest, "md5:" + android.digest); Reject([&] { ParseRelease(bad, "android.apk"); }, "bad digest accepted");
        bad = fixture; Change(bad, android.url, "https://example.org/package.apk"); Reject([&] { ParseRelease(bad, "android.apk"); }, "foreign download accepted");
        Reject([] { JsonReader("{\"tag\":1,\"tag\":2}").Read(); }, "duplicate key accepted");
        Reject([] { JsonReader("{\"x\":01}").Read(); }, "invalid JSON integer accepted");
        Check(JsonReader("{\"notes\":\"\\u4f20\\ud83d\\ude80\"}").Read().At("notes").text == "传🚀", "Unicode release notes");
        Reject([] { JsonReader("{\"notes\":\"\\ud800x\"}").Read(); }, "unpaired surrogate accepted");
        const auto source = fs::absolute(argv[1]), target = root / L"Chuan.exe";
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        const auto currentVersion = Utf8(Version);
        const auto digest = FileHash(source); VerifyExecutable(source, currentVersion); ++checks;
        Reject([&] { VerifyExecutable(source, "V0.0.0"); }, "mismatched executable version accepted");
        const std::wstring transaction = L"{00000000-0000-0000-0000-000000000001}";
        Reject([&] { ReplaceExecutable(source, target, std::string(64, '0'), currentVersion, transaction); }, "tampered executable accepted");
        Check(FileHash(target) == digest, "bad digest changed installed program");
        HANDLE held = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "lock installed program");
        Reject([&] { ReplaceExecutable(source, target, digest, currentVersion, transaction); }, "locked program replaced"); CloseHandle(held);
        Check(FileHash(target) == digest, "failed replacement changed program");
        ReplaceExecutable(source, target, digest, currentVersion, transaction);
        Check(FileHash(target) == digest && FileHash(root / (L".Chuan-backup-" + transaction + L".exe")) == digest, "atomic replacement or backup failed");
        Reject([&] { ReplaceExecutable(source, target, digest, currentVersion, transaction); }, "existing backup overwritten");
        if (argc > 4 && std::wstring(argv[4]) == L"--live") {
            Manager manager; manager.Check(); const auto end = GetTickCount64() + 60000;
            while (manager.Get().phase == Phase::Checking && GetTickCount64() < end) Sleep(50);
            auto state = manager.Get(); if (state.phase != Phase::Current && state.phase != Phase::Available) std::wcerr << state.message << L'\n';
            Check(state.phase == Phase::Current || state.phase == Phase::Available, "live GitHub version check failed");
            const auto archive = root / L"current-release.zip"; std::atomic<bool> cancel{false}; uint64_t received = 0;
            FetchPackage(desktop, archive, cancel, [&](uint64_t n) { received = n; });
            Check(received == desktop.size && FileHash(archive) == desktop.digest, "live package download");
            const auto extracted = root / L"extracted"; fs::create_directory(extracted); PreparePackage(desktop, archive, extracted);
            Check(fs::exists(extracted / L"Chuan.exe"), "validated extraction");
            cancel = true; const auto partial = root / L"cancelled.part";
            Reject([&] { FetchPackage(desktop, partial, cancel, [](uint64_t) {}); }, "cancelled download succeeded");
            Check(!fs::exists(partial), "cancelled download retained partial file");
        }
        std::cout << checks << " updater checks passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
