#pragma once
#include "pipeline.h"
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
#include <semaphore>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace chuan {
namespace diagnostic { class Log; }
namespace archive { struct Artifact; struct Plan; }
inline constexpr wchar_t Version[] = L"2.1.1";
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
fs::path AndroidPackagePath();

struct Config {
    std::wstring name;
    std::wstring id;
    fs::path receiveDir;
    fs::path dataDir;
    fs::path fallbackDataDir; // Used only when the default user storage is unavailable.
    bool automaticStorageFallback = false;
    unsigned short port = DefaultPort;
    unsigned short discoveryPort = DefaultDiscoveryPort;
    bool autoReceive = true;
    bool shareSoftware = true;
    bool zipFolders = false;
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
    bool directoryReceive = false;
    std::string gateway;
    std::wstring owner;
    bool reachable = true;
    std::string platform;
};
inline std::wstring PeerLabel(const Peer &peer){if(peer.browser)return peer.platform=="mobile-web"?L"手机 Web":peer.platform=="desktop-web"?L"电脑 Web":L"Web";return peer.platform=="windows"?L"Win 客户端":peer.platform=="android"?L"Android 端":L"客户端";}
inline constexpr uint64_t BrowserPresenceGrace = 5ull * 60 * 1000;
inline bool PeerSeenRecently(const Peer &peer,uint64_t now){return now-peer.seen<(peer.browser&&peer.owner.empty()?BrowserPresenceGrace:6000);}
enum class State { Queued, Waiting, Running, Completed, Cancelled, Failed, Rejected };
std::wstring StateText(State state, bool receiving);
struct OfferItem {
    std::wstring name;
    bool folder = false;
    uint64_t count = 1, total = 0;
};
struct Transfer {
    uint64_t id = 0, total = 0, done = 0;
    std::wstring name, peer, error, activity;
    fs::path savedPath;
    bool receiving = false;
    bool directory = false, skipped = false, folderStopped = false;
    bool awaitingApproval = false;
    bool removed = false;
    bool requestOnly = false;
    bool canRetry = false;
    bool paused = false;
    std::vector<OfferItem> offerItems;
    std::string offerSelection;
    std::wstring folderName;
    std::string folderId;
    uint64_t folderCount = 0, folderTotal = 0;
    State state = State::Queued;
    double speed = 0;
    uint64_t started = 0;
};
inline std::wstring TransferText(const Transfer &job){return job.skipped ? L"内容相同，已跳过" : job.state<State::Completed&&!job.activity.empty()?job.activity:StateText(job.state,job.receiving);}
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
    fs::path ExportDiagnostics(const std::string &client = {});
    bool UpdateConfig(const Config &config, std::wstring &error);
    void Refresh();
    bool AddPeer(const std::string &address, std::wstring &error);
    void SendFiles(const Peer &peer, const std::vector<fs::path> &files,bool forceZip=false);
    bool RetryTransfer(uint64_t id, std::wstring &error);
    void Cancel(uint64_t id);
    void Pause(uint64_t id, bool paused);
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
        std::atomic<bool> paused{false};
        bool remotePaused=false;
        bool resumeReset=false;
        int decision = 0;
        std::condition_variable cv;
        std::string webClient, webUpload, webTab;
        bool browserPaused=false;
        std::wstring resumeActivity;
        std::string nativeOwner;
        fs::path archiveSource;
        std::shared_ptr<archive::Artifact> archiveFile;
        std::shared_ptr<archive::Plan> archivePlan;
        std::string folderId, folderKey;
        std::string batchId, selection, asyncOfferKey;
        size_t batchItem = 0;
        std::string recipient;
        BY_HANDLE_FILE_INFORMATION fileIdentity{};
        bool hasIdentity = false;
        bool verificationPending = false;
        bool staging = false;
        bool hidden = false;
        bool diagnosticReported = false;
        int traceState=-1;std::wstring tracePhase,traceError;
        uint64_t traceAt=0,traceCreated=0,traceSample=0,traceBytes=0;
        bool announced = false;
        uint64_t finishedAt = 0,progressBase=0;
        std::atomic<uint64_t> retryDeadline{0};
        std::atomic<bool> retryExpired{false};
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
    std::unique_ptr<diagnostic::Log> diagnostic_;
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
        bool announced = false;
        std::set<size_t> cancelledItems;
        std::set<size_t> announcedItems;
    };
    std::map<std::string, BatchApproval> batchApprovals_;
    struct WebArchiveStream {
        SOCKET socket=INVALID_SOCKET;std::mutex mutex;std::atomic<uint64_t> touched{0};
        std::string client,tab;uint64_t total=0,received=0,offset=0;bool direct=false;
        std::shared_ptr<Job> job;std::function<void(SOCKET)> close;
        ~WebArchiveStream(){if(socket!=INVALID_SOCKET&&close)close(socket);}
    };
    std::map<std::string,std::shared_ptr<WebArchiveStream>> webArchiveStreams_;
    void WebArchive(SOCKET socket,const std::map<std::string,std::string> &headers,const std::string &ip,const std::string &path);
    void ResumeProbe(SOCKET socket,const std::map<std::string,std::string> &headers,const std::string &ip,bool browser);

    void ReceiveOffer(SOCKET socket, const std::map<std::string, std::string> &headers, const std::string &ip, bool browser);
    void CompleteOfferLocked(const std::shared_ptr<Job> &job, const std::string &selection);
    void ReceiveOfferStatus(SOCKET socket, const std::map<std::string, std::string> &headers, const std::string &ip, bool browser, bool cancel);
    void ReceiveBatchFiles(SOCKET socket, const std::map<std::string, std::string> &headers, const std::string &ip, bool status, bool browser=true);
    void ReceivePhase(SOCKET socket,const std::map<std::string,std::string> &headers,const std::string &ip,const std::string &upload,bool browser);
    void RetryUpload(SOCKET socket,const std::map<std::string,std::string> &headers,const std::string &ip,const std::string &upload,bool browser);
    void QueueRetry(const Peer &peer,const fs::path &path,const std::shared_ptr<Job> &job);
    struct RetryItem {Peer peer;fs::path path;std::shared_ptr<Job> job;};
    std::vector<RetryItem> retryQueue_;
    std::thread retryThread_,retryTimerThread_;
    std::condition_variable retryCv_;
    void RetryLoop();
    int BatchCommand(const Peer &peer,const std::string &path,const std::string &batch,const std::string &body,std::string &response,const std::shared_ptr<Job> &job);
    bool SendOffer(const Peer &peer, const std::string &batch, const std::vector<OfferItem> &items,
                   std::string &selection, std::wstring &error, const std::shared_ptr<Job> &control = {},
                   SOCKET waitingSocket = INVALID_SOCKET,
                   const std::function<void(const std::string &)> &progress = {},
                   const std::function<std::string()> &cancelled = {});
    std::string webToken_;
    struct BrowserSession {
        std::wstring id, name;
        bool directoryReceive=false;
        std::string platform;
        std::string token, ip;
        std::map<std::string, uint64_t> tabs;
        std::set<std::string> openTabs;
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
    std::counting_semaphore<FileProbeLimit> preparationSlots_{FileProbeLimit};
    std::counting_semaphore<FileVerificationLimit> verificationSlots_{FileVerificationLimit};
    std::counting_semaphore<FileTransferLimit> transferSlots_{FileTransferLimit};
    std::atomic<unsigned> diagnosticPreparing_{0},diagnosticVerifying_{0},diagnosticTransferring_{0};
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
    void WebDownload(SOCKET socket, const std::string &ip, uint64_t id, const std::string &client = {},bool checksum=false,uint64_t offset=0,const std::string &prefix={},bool range=false);
    void QueueBrowser(const Peer &peer, const fs::path &path, const std::shared_ptr<Job> &job,
                      const fs::path &root = {});
    void RememberFile(const std::shared_ptr<Job> &job, const fs::path &path);
    void ObserveNative(const std::map<std::string, std::string> &headers, const std::string &ip);
    enum class SendStage { All, Prepare, Probe, Verify, Transfer };
    void SendOne(const Peer &peer, const fs::path &path, const std::wstring &relative,
                 const std::shared_ptr<Job> &job, const fs::path &webRoot = {}, SendStage stage = SendStage::All);
    std::shared_ptr<Job> NewJob(const std::wstring &name, const std::wstring &peer, uint64_t total,
                                bool receiving);
    void SetState(const std::shared_ptr<Job> &job, State state, const std::wstring &error = L"");
    void TraceJobLocked(const std::shared_ptr<Job> &job, bool sample=false);
    void Progress(const std::shared_ptr<Job> &job, uint64_t done);
    void TrackSocket(SOCKET socket, const std::shared_ptr<Job> &job = {});
    void ReleaseSocket(SOCKET socket);
    bool IsLocal(const std::string &ip) const;
    void Notice(const std::wstring &value);
    std::set<std::string> BusyFoldersLocked();
    std::string Advertisement();
    void ReceiveUpload(SOCKET socket, const std::map<std::string, std::string> &headers,
                       const std::string &ip, bool browser = false, bool probe = false);
};
} // namespace chuan
