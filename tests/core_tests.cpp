#include "core.h"
#include "transfer_view.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace chuan;
namespace {
unsigned short FreePort(int type) {
    SOCKET socketValue = socket(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socketValue, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR)
        throw std::runtime_error("test port allocation");
    int length = sizeof(address);
    getsockname(socketValue, reinterpret_cast<sockaddr *>(&address), &length);
    auto port = ntohs(address.sin_port);
    closesocket(socketValue);
    return port;
}
bool Eventually(const std::function<bool()> &predicate) {
    for (int i = 0; i < 200; ++i) {
        if (predicate())
            return true;
        Sleep(25);
    }
    return false;
}
SOCKET TestConnect(unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR)
        throw std::runtime_error("test connection");
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    return s;
}
std::string TestReply(SOCKET s) {
    std::string result;
    char buffer[4096];
    int count = 0;
    while ((count = recv(s, buffer, sizeof(buffer), 0)) > 0) result.append(buffer, count);
    closesocket(s);
    return result;
}
std::string JsonString(const std::string &response, const std::string &key) {
    const auto marker = "\"" + key + "\":\"";
    auto start = response.find(marker);
    if (start == std::string::npos) throw std::runtime_error("missing JSON field");
    start += marker.size();
    return response.substr(start, response.find('"', start) - start);
}
std::string TestGet(unsigned short port, const std::string &path, const std::string &headers = {}) {
    SOCKET s = TestConnect(port);
    auto request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                   "\r\nConnection: close\r\n" + headers + "\r\n";
    send(s, request.data(), static_cast<int>(request.size()), 0);
    std::string result;
    char buffer[4096];
    int count = 0;
    while ((count = recv(s, buffer, sizeof(buffer), 0)) > 0)
        result.append(buffer, count);
    closesocket(s);
    return result;
}
} // namespace
int main() {
    int count = 0;
    auto check = [&](bool value, const char *label) {
        ++count;
        if (!value)
            throw std::runtime_error(label);
    };
    try {
        for (auto name : {L"../secret", L"a/../../secret", L"C:\\secret", L"\\\\server\\file", L"CON.txt",
                          L"NUL", L"COM1.txt", L"LPT9.log", L"file:stream", L"x. ", L"a//b", L"/absolute",
                          L"a/", L"a\n.txt", L"CON .txt"})
            check(SanitizeRelative(name).empty(), "unsafe filename accepted");
        check(SanitizeRelative(L"资料/中文文件.zip") == L"资料\\中文文件.zip", "unicode directory");
        check(SanitizeRelative(L"readme.pdf") == L"readme.pdf", "regular name");
        check(Wide(Utf8(L"中文 😀 文件")) == L"中文 😀 文件", "utf8 round trip");
        check(Unhex(Hex(Utf8(L"中文.zip"))) == Utf8(L"中文.zip"), "hex round trip");
        check(Unhex("gg").empty() && Unhex("a").empty(), "invalid hex");
        unsigned short p = 0;
        check(ValidPort(L"45871", p) && p == 45871, "port");
        check(!ValidPort(L"0", p) && !ValidPort(L"65536", p) && !ValidPort(L"-1", p) && !ValidPort(L"12x", p),
              "invalid ports");
        check(FormatBytes(0) == L"0 B", "zero bytes");
        Transfer first; first.id=1; first.folderId="batch"; first.folderName=L"资料"; first.name=L"资料\\a.txt";
        first.folderCount=2; first.folderTotal=100; first.total=40; first.done=40; first.speed=999; first.state=State::Completed;
        first.savedPath=L"C:\\received\\资料\\a.txt";
        Transfer second=first; second.id=2; second.name=L"资料\\子目录\\b.txt"; second.total=60; second.done=20; second.speed=12; second.state=State::Running;
        auto rows=TransferRows({first,second},{});
        check(rows.size()==1 && rows[0].folderRow && rows[0].total==100 && rows[0].done==60 && rows[0].speed==12 && rows[0].completedFiles==1,
              "collapsed native folder aggregates bytes and live speed without stale completed speed");
        check(rows[0].savedPath==fs::path(L"C:\\received\\资料"), "folder parent locates its root directory");
        auto expanded=TransferRows({first,second},{rows[0].group});
        check(expanded.size()==1 && expanded[0].folderRow, "native folder remains atomic even when expansion is requested");
        check(TransferRows({first},{})[0].state==State::Waiting, "partially arrived folder is not completed");
        auto other=first; other.id=3; other.folderId="another-batch";
        check(TransferRows({first,other},{}).size()==2, "same-name native folders keep independent batch identities");
        other.folderId=first.folderId; other.receiving=true;
        check(TransferRows({first,other},{}).size()==2, "native send and receive groups remain separate");
        auto removedFirst=first; removedFirst.removed=true;
        auto completedSecond=second; completedSecond.state=State::Completed; completedSecond.done=60;
        auto remaining=TransferRows({removedFirst,completedSecond},{TransferGroup(first)});
        check(remaining.size()==1 && remaining[0].state==State::Completed && remaining[0].done==100 && remaining[0].members.size()==1,
              "deleting child retains complete folder accounting and hides only that child");
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        fs::path work = fs::current_path() / (L"core-scenarios-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(work);
        auto startupConfig = [&](const wchar_t *label) {
            auto config = LoadConfig(work / label);
            config.port = FreePort(SOCK_STREAM);
            config.discoveryPort = FreePort(SOCK_DGRAM);
            config.receiveDir = work / (std::wstring(label) + L"-received");
            return config;
        };
        {
            auto config = startupConfig(L"startup-invalid-receive");
            std::ofstream(config.receiveDir) << "existing file must be preserved";
            SaveConfig(config);
            Engine engine(LoadConfig(config.dataDir));
            std::wstring error;
            check(engine.Start(error), "unavailable saved receive directory recovers at startup");
            const auto state = engine.GetSnapshot();
            check(error.empty() && state.config.receiveDir == config.dataDir / L"received" &&
                      state.notice.find(config.receiveDir.wstring()) != std::wstring::npos,
                  "startup recovery identifies original and replacement directory");
            check(LoadConfig(config.dataDir).receiveDir == state.config.receiveDir &&
                      fs::is_regular_file(config.receiveDir),
                  "recovered directory persists for older versions without removing original file");
            engine.Stop();
        }
        {
            auto config = startupConfig(L"startup-readonly-settings");
            SaveConfig(config);
            const auto ini = config.dataDir / L"settings.ini";
            check(SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_READONLY), "set readonly settings fixture");
            Engine engine(config);
            std::wstring error;
            const bool started = engine.Start(error);
            const auto state = engine.GetSnapshot();
            std::wstring updateError;
            auto changed = config;
            changed.name = L"unsaved name";
            const bool updated = engine.UpdateConfig(changed, updateError);
            SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
            check(started && error.empty() && state.notice.find(ini.wstring()) != std::wstring::npos,
                  "readonly configuration permits startup with precise warning");
            check(!updated && updateError.find(ini.wstring()) != std::wstring::npos &&
                      engine.GetSnapshot().config.name == config.name && LoadConfig(config.dataDir).name == config.name,
                  "failed settings update reports path and preserves active and saved settings");
            check(TestGet(config.port, "/api/info").starts_with("HTTP/1.1 200 "),
                  "service remains usable when settings cannot be saved");
            engine.Stop();
        }
        {
            auto config = startupConfig(L"startup-no-storage");
            std::ofstream(config.receiveDir) << "blocked receive";
            std::ofstream(config.dataDir) << "blocked configuration directory";
            Engine engine(config);
            std::wstring error;
            check(!engine.Start(error) && error.find(config.receiveDir.wstring()) != std::wstring::npos &&
                      error.find((config.dataDir / L"received").wstring()) != std::wstring::npos &&
                      error.find(L"系统错误") != std::wstring::npos,
                  "no writable receive directory fails with both paths and system error");
            config.fallbackDataDir = work / L"portable-config";
            Engine recovered(config);
            check(recovered.Start(error), "portable storage recovers when user directories are unavailable");
            const auto state = recovered.GetSnapshot();
            check(state.config.dataDir == config.fallbackDataDir &&
                      state.config.receiveDir == config.fallbackDataDir / L"received" &&
                      LoadConfig(config.fallbackDataDir).id == config.id &&
                      state.notice.find(L"便携配置") != std::wstring::npos,
                  "portable recovery saves original identity and reports the new storage location");
            recovered.Stop();
        }
        Config receiverConfig = LoadConfig(work / L"receiver-config");
        receiverConfig.port = FreePort(SOCK_STREAM);
        receiverConfig.discoveryPort = FreePort(SOCK_DGRAM);
        receiverConfig.receiveDir = work / L"received";
        receiverConfig.autoReceive = false;
        Engine receiver(receiverConfig);
        std::wstring error;
        check(receiver.Start(error), "manual receiver starts");
        Config senderConfig = LoadConfig(work / L"sender-config");
        senderConfig.port = FreePort(SOCK_STREAM);
        senderConfig.discoveryPort = FreePort(SOCK_DGRAM);
        senderConfig.receiveDir = work / L"sender-received";
        Engine sender(senderConfig);
        check(sender.Start(error), "sender starts");
        check(sender.AddPeer("127.0.0.1:" + std::to_string(receiverConfig.port), error), "manual connection");
        check(Eventually([&] { return !receiver.GetSnapshot().peers.empty(); }),
              "TCP connection discovers sender reciprocally without UDP");
        auto interfaces = receiver.GetSnapshot().interfaces;
        check(!interfaces.empty() &&
                  std::all_of(interfaces.begin(), interfaces.end(), [](auto &n) { return !n.name.empty(); }),
              "service addresses include adapter names");
        check(std::is_sorted(interfaces.begin(), interfaces.end(),
                             [](auto &a, auto &b) { return !a.virtualAdapter && b.virtualAdapter; }),
              "physical adapters precede virtual adapters");
        fs::path source = work / L"manual.txt";
        {
            std::ofstream file(source, std::ios::binary);
            file << "confirmation test";
        }
        auto peer = sender.GetSnapshot().peers.at(0);
        auto pendingId = [&]() -> uint64_t {
            for (auto &job : receiver.GetSnapshot().transfers)
                if (job.state == State::Waiting)
                    return job.id;
            return 0;
        };
        sender.SendFiles(peer, {source});
        check(Eventually([&] { return pendingId() != 0; }), "manual request displayed before payload");
        check(!fs::exists(receiverConfig.receiveDir / source.filename()),
              "manual request does not write file");
        receiver.Decide(pendingId(), true);
        check(sender.WaitForIdle(std::chrono::seconds(5)), "accepted sender settles");
        check(sender.GetSnapshot().transfers.at(0).state == State::Completed &&
                  fs::file_size(receiverConfig.receiveDir / source.filename()) == fs::file_size(source),
              "accepted request completes verified transfer");
        check(sender.GetSnapshot().transfers.at(0).savedPath == fs::absolute(source),
              "outgoing transfer retains source path for folder location");
        sender.ClearFinished();
        receiver.ClearFinished();
        sender.SendFiles(peer, {source});
        check(Eventually([&] { return pendingId() != 0; }), "second confirmation request");
        receiver.Decide(pendingId(), false);
        check(sender.WaitForIdle(std::chrono::seconds(5)) &&
                  sender.GetSnapshot().transfers.at(0).state == State::Rejected,
              "explicit rejection propagates to sender");
        sender.ClearFinished();
        receiver.ClearFinished();
        sender.SendFiles(peer, {source});
        check(Eventually([&] { return pendingId() != 0; }), "cancel test awaits confirmation");
        sender.Cancel(sender.GetSnapshot().transfers.at(0).id);
        check(sender.WaitForIdle(std::chrono::seconds(5)) &&
                  sender.GetSnapshot().transfers.at(0).state == State::Cancelled,
              "sender cancellation interrupts confirmation wait");
        check(Eventually([&] { return !receiver.HasActiveTransfers(); }),
              "receiver detects disconnected confirmation client");
        sender.ClearFinished();
        receiver.ClearFinished();
        receiverConfig.autoReceive = true;
        check(receiver.UpdateConfig(receiverConfig, error), "auto-receive can be enabled without restart");
        receiverConfig.autoReceive = false;
        check(receiver.UpdateConfig(receiverConfig, error), "folder manual approval enabled");
        fs::path folder = work / L"folder-approval";
        fs::create_directories(folder / L"child");
        { std::ofstream(folder / L"one.txt") << "one"; std::ofstream(folder / L"child" / L"two.txt") << "two"; }
        sender.SendFiles(peer, {folder});
        check(Eventually([&] {
            for (auto &job : receiver.GetSnapshot().transfers)
                if (job.awaitingApproval) return job.folderName == L"folder-approval" && job.folderCount == 2 && job.folderTotal == 6;
            return false;
        }), "folder prompt describes complete batch before receiving bytes");
        receiver.Decide(pendingId(), true);
        check(sender.WaitForIdle(std::chrono::seconds(5)) && fs::exists(receiverConfig.receiveDir / L"folder-approval" / L"child" / L"two.txt"),
              "one approval receives entire native folder including nested files");
        auto acceptedFolder = receiver.GetSnapshot();
        check(std::none_of(acceptedFolder.transfers.begin(), acceptedFolder.transfers.end(), [](auto &job) { return job.awaitingApproval; }), "no per-file approval remains");
        sender.ClearFinished(); receiver.ClearFinished();
        sender.SendFiles(peer, {folder});
        check(Eventually([&] { return pendingId() != 0; }), "new send of same folder requires fresh approval");
        receiver.Decide(pendingId(), false);
        check(sender.WaitForIdle(std::chrono::seconds(5)), "rejected folder settles without another prompt");
        auto rejectedFolder = sender.GetSnapshot();
        check(rejectedFolder.transfers.size() == 2 && std::all_of(rejectedFolder.transfers.begin(), rejectedFolder.transfers.end(), [](auto &job) { return job.state == State::Rejected || job.state == State::Cancelled; }), "folder rejection stops remaining files");
        sender.ClearFinished(); receiver.ClearFinished();
        std::vector<fs::path> loose;
        for (int i = 0; i < 50; ++i) { auto file = work / (L"batch-file-" + std::to_wstring(i) + L".txt"); std::ofstream(file) << i; loose.push_back(file); }
        sender.SendFiles(peer, loose);
        check(Eventually([&] { for (auto &job : receiver.GetSnapshot().transfers) if (job.awaitingApproval) return job.requestOnly && job.offerItems.size() == 50; return false; }), "50 files produce exactly one manifest prompt before payload");
        check(!fs::exists(receiverConfig.receiveDir / loose[0].filename()), "batch payload is not written before approval");
        std::string selected(50, '0'); selected[1] = selected[17] = selected[49] = '1';
        receiver.DecideItems(pendingId(), selected);
        check(sender.WaitForIdle(std::chrono::seconds(10)), "partial approval finishes without per-file prompts");
        for (size_t i = 0; i < loose.size(); ++i) check(fs::exists(receiverConfig.receiveDir / loose[i].filename()) == (selected[i] == '1'), "only selected payloads are received");
        sender.ClearFinished(); receiver.ClearFinished();
        auto many = work / L"more-than-5000"; fs::create_directory(many);
        for (int i = 0; i < 5001; ++i) { std::ofstream file(many / (std::to_wstring(i) + L".txt")); }
        sender.SendFiles(peer, {many});
        check(Eventually([&] { for (auto &job : receiver.GetSnapshot().transfers) if (job.awaitingApproval) return job.offerItems.size() == 1 && job.folderCount == 5001; return false; }), "default unlimited enumerates and offers a real 5001-file folder");
        receiver.Decide(pendingId(), false);
        check(sender.WaitForIdle(std::chrono::seconds(10)), "large folder can be rejected atomically");
        sender.ClearFinished(); receiver.ClearFinished();
        receiverConfig.maxTransferFiles = 2;
        check(receiver.UpdateConfig(receiverConfig, error), "receiver file count limit can be configured");
        sender.SendFiles(peer, {loose[0], loose[2], loose[3]});
        check(sender.WaitForIdle(std::chrono::seconds(5)) && sender.GetSnapshot().transfers[0].state == State::Failed && pendingId() == 0, "receiver limit rejects entire batch before approval");
        sender.ClearFinished(); receiver.ClearFinished(); receiverConfig.maxTransferFiles = 0; receiverConfig.maxTransferBytes = 1;
        check(receiver.UpdateConfig(receiverConfig, error), "receiver byte limit can be configured");
        sender.SendFiles(peer, {loose[0], loose[2]});
        check(sender.WaitForIdle(std::chrono::seconds(5)) && sender.GetSnapshot().transfers[0].state == State::Failed, "aggregate byte limit applies across loose files");
        sender.ClearFinished(); receiver.ClearFinished(); receiverConfig.maxTransferBytes = 0;
        SaveConfig(receiverConfig); auto persisted = LoadConfig(receiverConfig.dataDir);
        check(persisted.maxTransferFiles == 0 && persisted.maxTransferBytes == 0, "zero limits persist as unlimited");
        receiverConfig.autoReceive = true;
        check(receiver.UpdateConfig(receiverConfig, error), "auto receive restored after folder test");
        const std::string browserKey = "33333333333333333333333333333333";
        const auto browserHeaders =
            "X-Chuan-Client: " + browserKey + "\r\nX-Chuan-Name: " + Hex(Utf8(L"测试浏览器")) + "\r\n";
        TestGet(receiverConfig.port, "/web/api/state", browserHeaders);
        auto browserPeers = receiver.GetSnapshot().peers;
        auto browser =
            std::find_if(browserPeers.begin(), browserPeers.end(), [](auto &p) { return p.browser; });
        check(browser != browserPeers.end() && browser->name == L"测试浏览器",
              "browser heartbeat registers named online device");
        fs::path browserSource = work / L"browser-receive.txt";
        {
            std::ofstream file(browserSource, std::ios::binary);
            file << "browser receipt";
        }
        receiver.SendFiles(*browser, {browserSource});
        uint64_t browserJob = 0;
        check(Eventually([&] {
                  for (auto &j : receiver.GetSnapshot().transfers)
                      if (j.name == browserSource.filename() && j.state == State::Waiting) {
                          browserJob = j.id;
                          return true;
                      }
                  return false;
              }),
              "desktop offers file to online browser");
        check(!receiver.DeleteTransfer(browserJob, true, error) && fs::exists(browserSource),
              "active delivery cannot delete source file");
        check(TestGet(receiverConfig.port, "/web/files/" + std::to_string(browserJob))
                  .starts_with("HTTP/1.1 404 "),
              "browser delivery is not exposed to other clients");
        check(TestGet(receiverConfig.port, "/web/files/" + std::to_string(browserJob), browserHeaders)
                  .ends_with("browser receipt"),
              "browser receives desktop file through streaming download");
        check(TestGet(receiverConfig.port, "/web/api/state", browserHeaders).find("\"receiving\":true") !=
                  std::string::npos,
              "browser shows receiver perspective");
        check(receiver.DeleteTransfer(browserJob, true, error) && !fs::exists(browserSource),
              "delete removes completed record and corresponding source file");
        fs::path changedSource = work / L"changed-source.txt";
        {
            std::ofstream file(changedSource);
            file << "original";
        }
        receiver.SendFiles(*browser, {changedSource});
        uint64_t changedId = 0;
        check(Eventually([&] {
                  for (auto &j : receiver.GetSnapshot().transfers)
                      if (j.name == changedSource.filename() && j.state == State::Waiting) {
                          changedId = j.id;
                          return true;
                      }
                  return false;
              }),
              "changed source test offered");
        receiver.Cancel(changedId);
        {
            std::ofstream file(changedSource, std::ios::app);
            file << "replacement";
        }
        check(!receiver.DeleteTransfer(changedId, true, error) && fs::exists(changedSource),
              "modified source is preserved when deleting transfer");
        check(receiver.DeleteTransfer(changedId, false, error) && fs::exists(changedSource),
              "record-only deletion leaves source intact");
        sender.SendFiles(peer, {source});
        check(sender.WaitForIdle(std::chrono::seconds(5)) &&
                  sender.GetSnapshot().transfers.at(0).state == State::Completed,
              "auto-receive takes effect on next transfer");
        check(LoadConfig(receiverConfig.dataDir).autoReceive, "settings persist");
        receiver.ClearFinished();
        check(TestGet(receiverConfig.port, "/web/api/state").starts_with("HTTP/1.1 401 "), "anonymous state rejected");
        auto webState = TestGet(receiverConfig.port, "/web/api/state", "X-Chuan-Client: 11111111111111111111111111111111\r\n");
        check(webState.starts_with("HTTP/1.1 200 ") && webState.find("manual.txt") == std::string::npos,
              "native received files never become a public web library");
        size_t tokenStart = webState.find("\"token\":\"");
        check(tokenStart != std::string::npos, "web token available");
        tokenStart += 9;
        auto token = webState.substr(tokenStart, webState.find('"', tokenStart) - tokenStart);
        receiverConfig.autoReceive = false;
        check(receiver.UpdateConfig(receiverConfig, error), "web manual mode enabled");
        unsigned folderUpload = 0;
        auto webFolder = [&](unsigned short port, const std::string &auth, const std::string &batch,
                             const std::string &path, const std::wstring &target = L"", unsigned count = 2) {
            SOCKET socketValue = TestConnect(port);
            auto uploadKey = std::string(31, '0') + char('a' + folderUpload++);
            auto request = "POST /web/api/upload HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                "\r\nX-Chuan-Token: " + auth + "\r\nX-Chuan-Client: 11111111111111111111111111111111" +
                "\r\nX-Chuan-Upload: " + uploadKey + "\r\nX-Chuan-Path: " + Hex(path) +
                "\r\nX-Chuan-Sender: " + Hex("Web folder test") + "\r\nX-Chuan-Target: " + Hex(Utf8(target)) +
                "\r\nX-Chuan-Folder: " + batch + "\r\nX-Chuan-Folder-Name: " + Hex("web-batch") +
                "\r\nX-Chuan-Folder-Count: " + std::to_string(count) +
                "\r\nX-Chuan-Folder-Size: 0\r\nX-Chuan-Size: 0\r\nContent-Length: 32\r\n\r\n" +
                Unhex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
            send(socketValue, request.data(), static_cast<int>(request.size()), 0);
            return socketValue;
        };
        auto batch = std::string(32, 'a');
        SOCKET folderSocket = webFolder(receiverConfig.port, token, batch, "web-batch/one.txt");
        check(Eventually([&] {
            for (auto &job : receiver.GetSnapshot().transfers)
                if (job.awaitingApproval) return job.folderName == L"web-batch" && job.folderCount == 2;
            return false;
        }), "web folder shows one batch approval");
        receiver.Decide(pendingId(), true);
        check(TestReply(folderSocket).starts_with("HTTP/1.1 201 "), "first web folder file accepted");
        receiver.ClearFinished();
        check(receiver.GetSnapshot().transfers.size()==1, "clear finished retains first completed child while the folder is incomplete");
        check(TestReply(webFolder(receiverConfig.port, token, batch, "web-batch/child/two.txt")).starts_with("HTTP/1.1 201 ") &&
            fs::exists(receiverConfig.receiveDir / L"web-batch/child/two.txt"), "remaining web folder file requires no further approval");
        check(TestReply(webFolder(receiverConfig.port, token, batch, "web-batch/extra.txt", L"", 3)).starts_with("HTTP/1.1 409 "),
              "folder consent cannot be extended with changed batch metadata");
        receiver.ClearFinished();
        batch = std::string(32, 'b');
        folderSocket = webFolder(receiverConfig.port, token, batch, "web-batch/one.txt");
        check(Eventually([&] { return pendingId() != 0; }), "same web folder in a new batch needs new approval");
        receiver.Decide(pendingId(), false);
        check(TestReply(folderSocket).starts_with("HTTP/1.1 417 ") &&
            TestReply(webFolder(receiverConfig.port, token, batch, "web-batch/child/two.txt")).starts_with("HTTP/1.1 417 "),
              "web folder rejection applies to remaining files");
        receiver.ClearFinished(); sender.ClearFinished();
        const auto relayToken = JsonString(TestGet(senderConfig.port, "/web/api/state", "X-Chuan-Client: 11111111111111111111111111111111\r\n"), "token");
        // Reuse the same folder id on another origin: prior rejection must not leak across origins.
        for (unsigned index = 0; index < 2; ++index) {
            folderUpload = index; // fresh upload IDs on this service
            auto staged = TestReply(webFolder(senderConfig.port, relayToken, batch,
                index ? "web-batch/relay/two.txt" : "web-batch/relay/one.txt", receiverConfig.id));
            check(staged.starts_with("HTTP/1.1 201 "), "folder staging bypasses intermediary approval");
            SOCKET relaySocket = TestConnect(senderConfig.port);
            const auto forward = "POST /web/api/forward/" + JsonString(staged, "id") +
                " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(senderConfig.port) +
                "\r\nX-Chuan-Client: 11111111111111111111111111111111\r\nX-Chuan-Token: " + relayToken +
                "\r\nX-Chuan-Peer: " + Hex(Utf8(receiverConfig.id)) + "\r\nX-Chuan-Upload: " + std::string(31, '0') + char('a' + index) +
                "\r\nContent-Length: 0\r\n\r\n";
            send(relaySocket, forward.data(), static_cast<int>(forward.size()), 0);
            check(TestReply(relaySocket).starts_with("HTTP/1.1 202 "), "folder relay accepted");
            if (!index) {
                check(Eventually([&] {
                    for (auto &job : receiver.GetSnapshot().transfers)
                        if (job.awaitingApproval) return job.folderName == L"web-batch" && job.folderCount == 2;
                    return false;
                }), "relay preserves batch approval metadata at final receiver");
                receiver.Decide(pendingId(), true);
            }
            check(sender.WaitForIdle(std::chrono::seconds(5)), "folder relay completes under one approval");
        }
        check(fs::exists(receiverConfig.receiveDir / L"web-batch/relay/two.txt"), "relayed folder retains nested hierarchy");
        sender.ClearFinished(); receiver.ClearFinished();
        SOCKET webSocket = TestConnect(receiverConfig.port);
        std::string webRequest =
            "POST /web/api/upload HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(receiverConfig.port) +
            "\r\nX-Chuan-Token: " + token +
            "\r\nX-Chuan-Client: 11111111111111111111111111111111"
            "\r\nX-Chuan-Upload: 22222222222222222222222222222222\r\nX-Chuan-Path: " +
            Hex("web-pending.txt") + "\r\nX-Chuan-Sender: " + Hex("Web test") +
            "\r\nX-Chuan-Size: 3\r\nContent-Length: 35\r\n\r\nabc" + std::string(32, 0);
        send(webSocket, webRequest.data(), static_cast<int>(webRequest.size()), 0);
        check(Eventually([&] { return pendingId() != 0; }), "browser request awaits native confirmation");
        receiverConfig.shareSoftware = false;
        check(receiver.UpdateConfig(receiverConfig, error) &&
                  Eventually([&] { return !receiver.HasActiveTransfers(); }) &&
                  !fs::exists(receiverConfig.receiveDir / L"web-pending.txt"),
              "disabling web service cancels pending browser upload");
        closesocket(webSocket);
        check(TestGet(receiverConfig.port, "/web/api/state").starts_with("HTTP/1.1 403 ") &&
                  TestGet(receiverConfig.port, "/api/info").starts_with("HTTP/1.1 200 "),
              "web switch preserves native protocol");
        receiverConfig.autoReceive = true;
        check(receiver.UpdateConfig(receiverConfig, error), "native auto receive enabled with web off");
        sender.ClearFinished();
        sender.SendFiles(peer, {source});
        check(sender.WaitForIdle(std::chrono::seconds(5)) &&
                  sender.GetSnapshot().transfers.at(0).state == State::Completed,
              "native transfer works with web service disabled");
        Sleep(11000);
        check(!sender.GetSnapshot().peers.empty(),
              "manual connection remains online without shared broadcast");
        receiver.Stop();
        sender.Stop();
        WSACleanup();
        std::cout << count << " checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
