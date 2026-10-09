#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace chuan {
inline constexpr wchar_t Version[] = L"1.0.0";
inline constexpr unsigned short DefaultPort = 45871;
inline constexpr unsigned short DefaultDiscoveryPort = 45872;
namespace fs = std::filesystem;
std::string Utf8(const std::wstring &value);
std::wstring Wide(const std::string &value);
std::string Hex(const std::string &value);
std::string Unhex(const std::string &value);
std::wstring FormatBytes(uint64_t value);
std::wstring SanitizeRelative(const std::wstring &value);
bool ValidPort(const std::wstring &value, unsigned short &result);
fs::path ExePath();

struct Config {
    std::wstring name;
    std::wstring id;
    fs::path receiveDir;
    fs::path dataDir;
    fs::path fallbackDataDir; // Used only when the default user storage is unavailable.
    unsigned short port = DefaultPort;
    unsigned short discoveryPort = DefaultDiscoveryPort;
    bool autoReceive = true;
    bool shareSoftware = true;
    bool automaticName = false;
    uint64_t maxTransferFiles = 0, maxTransferBytes = 0; // Zero disables the business limit.
};
Config LoadConfig(const fs::path &dataDir = {});
void SaveConfig(const Config &config);
struct Peer {
    std::wstring id, name;
    std::string ip;
    unsigned short port = DefaultPort;
    uint64_t seen = 0;
    bool manual = false;
    bool browser = false;
    std::string gateway;
    std::wstring owner;
    bool reachable = true;
};
enum class State { Queued, Waiting, Running, Completed, Cancelled, Failed, Rejected };
std::wstring StateText(State state, bool receiving);
struct OfferItem {
    std::wstring name;
    bool folder = false;
    uint64_t count = 1, total = 0;
};
struct Transfer {
    uint64_t id = 0, total = 0, done = 0;
    std::wstring name, peer, error;
    fs::path savedPath;
    bool receiving = false;
    bool awaitingApproval = false;
    bool removed = false;
    bool requestOnly = false;
    std::vector<OfferItem> offerItems;
    std::wstring folderName;
    std::string folderId;
    uint64_t folderCount = 0, folderTotal = 0;
    State state = State::Queued;
    double speed = 0;
    uint64_t started = 0;
};
struct Snapshot {
    Config config;
    unsigned short boundPort = 0;
    std::vector<Peer> peers;
    std::vector<Transfer> transfers;
    std::vector<std::string> addresses;
    struct ServiceAddress {
        std::string ip;
        std::wstring name;
        bool virtualAdapter = false;
    };
    std::vector<ServiceAddress> interfaces;
    std::wstring notice;
    uint64_t ping = 0;
    struct WebInvite {
        uint64_t sequence = 0;
        std::wstring sender, url;
    } webInvite;
};

class Engine {
  public:
    explicit Engine(Config config);
    ~Engine();
    Engine(const Engine &) = delete;
    bool Start(std::wstring &error);
    void Stop();
    Snapshot GetSnapshot();
    bool UpdateConfig(const Config &config, std::wstring &error);
    void Refresh();
    bool AddPeer(const std::string &address, std::wstring &error);
    void SendFiles(const Peer &peer, const std::vector<fs::path> &files);
    void Cancel(uint64_t id);
    void Decide(uint64_t id, bool accept);
    void DecideItems(uint64_t id, const std::string &selection);
    bool HasActiveTransfers();
    void ClearFinished();
    bool DeleteTransfer(uint64_t id, bool removeFile, std::wstring &error);
    bool WaitForIdle(std::chrono::seconds timeout);
    void Ping(const Peer &peer);
    void InviteWeb(const Peer &peer);
    void SetNote(const std::string &ip, const std::wstring &note);
    std::wstring GetNote(const std::string &ip);

  private:
    struct Job {
        Transfer view;
        std::atomic<bool> cancel{false};
        int decision = 0;
        std::condition_variable cv;
        std::string webClient, webUpload;
        std::string folderId, folderKey;
        std::string batchId, selection;
        size_t batchItem = 0;
        std::string recipient;
        BY_HANDLE_FILE_INFORMATION fileIdentity{};
        bool hasIdentity = false;
        bool staging = false;
        bool hidden = false;
        uint64_t finishedAt = 0;
        std::wstring targetId;
        std::wstring senderName;
        std::set<std::string> hiddenClients;
    };
    struct WebFile {
        fs::path path, root;
        uint64_t size;
        std::string recipient;
        std::shared_ptr<Job> delivery;
        std::string owner;
        BY_HANDLE_FILE_INFORMATION identity{};
        bool hasIdentity = false;
        std::wstring relative;
    };
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    struct Network {
        uint32_t address, mask, broadcast;
        std::string text;
        std::wstring name;
        bool virtualAdapter;
        unsigned priority;
    };
    Config config_;
    unsigned short boundPort_ = 0;
    std::wstring notice_;
    std::mutex mutex_, workersMutex_, socketsMutex_;
    std::vector<Peer> peers_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::map<uint64_t, WebFile> webFiles_;
    struct FolderApproval {
        std::wstring name;
        uint64_t count = 0, total = 0, reserved = 0, prompt = 0;
        int decision = 0;
        std::set<std::wstring> paths;
    };
    std::map<std::string, FolderApproval> folderApprovals_;
    struct BatchApproval {
        std::vector<OfferItem> items;
        std::string selection, target, recipient;
        std::vector<uint64_t> counts, bytes;
        std::set<std::pair<size_t, std::wstring>> paths;
        uint64_t touched = 0;
    };
    std::map<std::string, BatchApproval> batchApprovals_;
    void ReceiveOffer(SOCKET socket, const std::map<std::string, std::string> &headers, const std::string &ip, bool browser);
    bool SendOffer(const Peer &peer, const std::string &batch, const std::vector<OfferItem> &items,
                   std::string &selection, std::wstring &error, const std::shared_ptr<Job> &control = {});
    std::string webToken_;
    struct BrowserSession {
        std::wstring id, name;
        std::string token, ip;
        std::map<std::string, uint64_t> tabs;
        uint64_t ping = 0, ack = 0, pingAt = 0;
        uint64_t maxFiles = 0, maxBytes = 0;
        std::map<std::string, std::wstring> notes;
    };
    std::map<std::string, BrowserSession> browsers_;
    std::map<std::string, uint64_t> aliases_;
    uint64_t nextAlias_ = 1, ping_ = 0;
    Snapshot::WebInvite webInvite_;
    uint64_t lastWebInviteAt_ = 0, lastWebInviteSentAt_ = 0;
    std::map<std::string, std::wstring> notes_;
    std::set<std::wstring> probing_;
    std::map<std::string, uint64_t> pingTimes_;
    std::wstring AutoNameLocked(const std::string &key);
    void ExpireLocked();
    void CleanRelay();
    void SyncMesh(const Peer &peer);
    std::string Mesh(const std::string &ledger);
    bool RequestPeer(const Peer &peer, const std::string &path, const std::string &extra, std::string &body);
    bool PingNow(const Peer &peer, std::wstring &error);
    std::vector<Worker> workers_;
    std::map<SOCKET, std::weak_ptr<Job>> sockets_;
    std::vector<Network> networks_;
    std::atomic<bool> running_{false};
    std::atomic<bool> refresh_{false};
    SOCKET listener_ = INVALID_SOCKET, discovery_ = INVALID_SOCKET;
    std::thread serverThread_, discoveryThread_;
    uint64_t nextJob_ = 1;
    bool winsock_ = false;
    bool Launch(std::function<void()> fn);
    void CollectWorkers();
    void ServerLoop();
    void DiscoveryLoop();
    void HandleClient(SOCKET socket, const std::string &ip);
    bool HandleWeb(SOCKET socket, const std::string &ip, const std::string &method, const std::string &path,
                   const std::map<std::string, std::string> &headers);
    void WebDownload(SOCKET socket, const std::string &ip, uint64_t id, const std::string &client = {});
    void QueueBrowser(const Peer &peer, const fs::path &path, const std::shared_ptr<Job> &job,
                      const fs::path &root = {});
    void RememberFile(const std::shared_ptr<Job> &job, const fs::path &path);
    void ObserveNative(const std::map<std::string, std::string> &headers, const std::string &ip);
    void SendOne(const Peer &peer, const fs::path &path, const std::wstring &relative,
                 const std::shared_ptr<Job> &job, const fs::path &webRoot = {});
    std::shared_ptr<Job> NewJob(const std::wstring &name, const std::wstring &peer, uint64_t total,
                                bool receiving);
    void SetState(const std::shared_ptr<Job> &job, State state, const std::wstring &error = L"");
    void Progress(const std::shared_ptr<Job> &job, uint64_t done);
    void TrackSocket(SOCKET socket, const std::shared_ptr<Job> &job = {});
    void ReleaseSocket(SOCKET socket);
    bool IsLocal(const std::string &ip) const;
    void Notice(const std::wstring &value);
    std::set<std::string> BusyFoldersLocked();
    std::string Advertisement();
    void ReceiveUpload(SOCKET socket, const std::map<std::string, std::string> &headers,
                       const std::string &ip, bool browser = false);
};
} // namespace chuan
