#pragma once
#include "core.h"
#include "update_manifest.h"

namespace chuan::update {
enum class Phase { Idle, Checking, Current, Available, Downloading, Ready, Error };
struct Status {
    Phase phase = Phase::Idle;
    std::wstring message = L"当前版本 V" + std::wstring(Version), version, notes;
    uint64_t done = 0, total = 0;
    bool settingsUnread = false, updateUnread = false;
};
struct Notices {
    std::wstring version, settingsSeen, updateSeen;
    void Found(const std::wstring &value) { version = value; }
    void SettingsRead() { settingsSeen = version; }
    void UpdateRead() { updateSeen = version; }
    bool SettingsUnread() const { return !version.empty() && settingsSeen != version; }
    bool UpdateUnread() const { return !version.empty() && updateSeen != version; }
};
class Manager {
    mutable std::mutex mutex_;
    Status status_;
    Notices notices_;
    Release release_;
    fs::path stage_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
    void Set(Phase phase, const std::wstring &message);
    void Join();
  public:
    ~Manager();
    Status Get() const;
    void SettingsRead();
    void UpdateRead();
    void Check();
    void Download();
    void Cancel();
    bool Restart(HWND owner, const fs::path &dataDir, std::wstring &error);
};
// Runs before configuration loading, network services, and the single-instance guard.
int RunHelper(const std::map<std::wstring, std::wstring> &options);
std::string FileHash(const fs::path &path);
void VerifyExecutable(const fs::path &path, const std::string &version);
void FetchPackage(const Release &release, const fs::path &file, std::atomic<bool> &cancel,
                  const std::function<void(uint64_t)> &progress);
void PreparePackage(const Release &release, const fs::path &archive, const fs::path &stage);
void ReplaceExecutable(const fs::path &source, const fs::path &target, const std::string &digest,
                       const std::string &version, const std::wstring &transaction);
} // namespace chuan::update
