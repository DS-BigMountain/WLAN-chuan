// Focused receive-state, consent and browser relay resume regressions.
#include "core.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace chuan;

static void Require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static unsigned short Port(int kind) {
    SOCKET s = socket(AF_INET, kind, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(bind(s, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "allocate port");
    int length = sizeof(address);
    getsockname(s, reinterpret_cast<sockaddr *>(&address), &length);
    closesocket(s); return ntohs(address.sin_port);
}
struct Connection {
    SOCKET socket;
    explicit Connection(unsigned short port) : socket(::socket(AF_INET, SOCK_STREAM, 0)) {
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        Require(connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "connect");
        DWORD timeout = 5000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
    }
    ~Connection() { closesocket(socket); }
    void Send(const std::string &value) {
        for (size_t at = 0; at < value.size();) {
            const int sent = send(socket, value.data() + at, static_cast<int>(value.size() - at), 0);
            Require(sent > 0, "send"); at += sent;
        }
    }
    std::string Header() {
        std::string result; char byte;
        while (!result.ends_with("\r\n\r\n")) {
            Require(recv(socket, &byte, 1, 0) == 1, "read response header"); result += byte;
        }
        return result;
    }
};
static std::string Request(unsigned short port, const std::string &method, const std::string &path,
                           const std::string &fields = {}, const std::string &body = {}) {
    Connection connection(port);
    connection.Send(method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
        "\r\n" + fields + "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    std::string result; char buffer[4096];
    for (int count; (count = recv(connection.socket, buffer, sizeof(buffer), 0)) > 0;) result.append(buffer, count);
    return result;
}
template<class Check> static void Until(Check check) {
    for (int i = 0; i < 300; ++i) { if (check()) return; Sleep(10); }
    throw std::runtime_error("condition timeout");
}
static std::string WebJobs(const std::string &response) {
    const auto start = response.find("\"jobs\":[");
    Require(response.starts_with("HTTP/1.1 200") && start != std::string::npos, "browser state response");
    return response.substr(start);
}
static void SingleWebJob(const std::string &response, uint64_t id, State state) {
    const auto jobs = WebJobs(response);
    const auto first = jobs.find("\"id\":");
    Require(first != std::string::npos && jobs.find("\"id\":", first + 5) == std::string::npos, "exactly one browser transfer");
    Require(jobs.find("\"id\":\"" + std::to_string(id) + "\"") != std::string::npos &&
        jobs.find("\"receiving\":true") != std::string::npos &&
        jobs.find("\"state\":" + std::to_string(static_cast<int>(state))) != std::string::npos,
        "browser shows the actual delivery as receiving with its current state");
}
int wmain(int argc, wchar_t **argv) {
    try {
        Require(argc == 2, "isolated output directory required");
        WSADATA sockets{}; WSAStartup(MAKEWORD(2, 2), &sockets);
        const auto root = fs::absolute(argv[1]) / std::to_wstring(GetCurrentProcessId());
        Config config; config.id = L"receive-regression"; config.name = L"review";
        config.dataDir = root / L"data"; config.receiveDir = root / L"received";
        config.port = Port(SOCK_STREAM); config.discoveryPort = Port(SOCK_DGRAM);
        Engine engine(config); std::wstring error;
        Require(engine.Start(error), "start receiver");
        const auto host = " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(config.port) + "\r\n";
        const auto sender = "X-Chuan-Id: " + Hex("regression-sender") + "\r\nX-Chuan-Sender: " + Hex("review") + "\r\n";
        {
            Connection connection(config.port);
            connection.Send("POST /api/upload" + host + sender + "X-Chuan-Path: " + Hex("huge.bin") +
                "\r\nX-Chuan-Size: 9000000000000000\r\nExpect: 100-continue\r\nContent-Length: 9000000000000032\r\n\r\n");
            Require(connection.Header().starts_with("HTTP/1.1 507"), "insufficient space response");
        }
        auto job = engine.GetSnapshot().transfers.front();
        Require(job.state == State::Failed && !job.error.empty() && !engine.HasActiveTransfers(), "space failure must terminate task");
        engine.ClearFinished();
        Require(engine.GetSnapshot().transfers.empty(), "space failure can be cleared");
        std::cout << "PASS insufficient storage terminates and clears the receive task\n";

        const std::string client(32, 'a');
        Require(Request(config.port, "GET", "/web/api/state", "X-Chuan-Client: " + client + "\r\n").starts_with("HTTP/1.1 200"), "register browser");
        std::wstring recipient;
        for (const auto &peer : engine.GetSnapshot().peers) if (peer.browser) recipient = peer.id;
        Require(!recipient.empty(), "browser identity");
        config.autoReceive = false; Require(engine.UpdateConfig(config, error), "disable automatic receive");
        const auto route = "X-Chuan-Recipient: " + Hex(Utf8(recipient)) + "\r\n";
        const auto direct = sender + route + "X-Chuan-Path: " + Hex("unapproved.txt") + "\r\nX-Chuan-Size: 0\r\n";
        {
            Connection connection(config.port);
            connection.Send("POST /api/upload" + host + direct + "Expect: 100-continue\r\nContent-Length: 32\r\n\r\n");
            Require(connection.Header().starts_with("HTTP/1.1 409"), "browser payload without batch must be rejected");
        }
        Require(!fs::exists(config.dataDir / L"relay" / L"unapproved.txt") && !engine.HasActiveTransfers(), "unapproved payload has no side effects");
        std::cout << "PASS browser payload cannot bypass batch approval\n";

        const auto browserState = Request(config.port, "GET", "/web/api/state", "X-Chuan-Client: " + client + "\r\n");
        const auto tokenAt = browserState.find("\"token\":\"");
        Require(tokenAt != std::string::npos, "local browser session");
        const auto tokenStart = tokenAt + 9, tokenEnd = browserState.find('"', tokenStart);
        const auto localBrowserFields = "X-Chuan-Client: " + client + "\r\nX-Chuan-Token: " +
            browserState.substr(tokenStart, tokenEnd - tokenStart) + "\r\n";
        Peer localBrowser;
        for (const auto &peer : engine.GetSnapshot().peers) if (peer.id == recipient) localBrowser = peer;
        const auto source = root / L"local-browser.txt";
        std::ofstream(source) << "local browser payload";
        for (const bool cancel : {false, true}) {
            engine.SendFiles(localBrowser, {source});
            Until([&] { for (const auto &item : engine.GetSnapshot().transfers)
                if (item.requestOnly && item.state == State::Waiting && item.name == source.filename()) {
                    engine.DecideItems(item.id, "1"); return true;
                } return false; });
            uint64_t delivery = 0;
            Until([&] { for (const auto &item : engine.GetSnapshot().transfers)
                if (!item.requestOnly && !item.receiving && item.name == source.filename() && item.activity == L"等待浏览器接收") {
                    delivery = item.id; return true;
                } return false; });
            auto snapshot = engine.GetSnapshot();
            Require(snapshot.transfers.size() == 1 && snapshot.transfers.front().id == delivery && !snapshot.transfers.front().receiving,
                "host shows only its outgoing delivery, not browser consent as self-receive");
            SingleWebJob(Request(config.port, "GET", "/web/api/state", localBrowserFields), delivery, State::Waiting);
            const std::string outsider(32, '9');
            Require(WebJobs(Request(config.port, "GET", "/web/api/state", "X-Chuan-Client: " + outsider + "\r\n")) == "\"jobs\":[]}",
                "another browser cannot see delivery or consent placeholder");
            if (cancel) {
                Require(Request(config.port, "POST", "/web/api/cancel/" + std::to_string(delivery), localBrowserFields).starts_with("HTTP/1.1 200"),
                    "browser cancellation addresses the active delivery");
            } else {
                Require(Request(config.port, "GET", "/web/files/" + std::to_string(delivery) + "?client=" + client).ends_with("\r\n\r\nlocal browser payload"),
                    "local browser receives exact contents");
            }
            const auto finalState = cancel ? State::Cancelled : State::Completed;
            Until([&] { const auto current = engine.GetSnapshot(); return current.transfers.size() == 1 &&
                current.transfers.front().id == delivery && current.transfers.front().state == finalState; });
            SingleWebJob(Request(config.port, "GET", "/web/api/state", localBrowserFields), delivery, finalState);
            Require(Request(config.port, "POST", "/web/api/clear", localBrowserFields).starts_with("HTTP/1.1 200"), "clear browser history");
            Require(WebJobs(Request(config.port, "GET", "/web/api/state", localBrowserFields)) == "\"jobs\":[]}", "clearing history cannot restore consent placeholder");
            engine.ClearFinished();
            Require(engine.GetSnapshot().transfers.empty(), "host history clears without self-receive placeholder");
        }
        std::cout << "PASS local host-to-browser direction, unique task, download, cancellation, history clearing and session isolation\n";

        const std::string batch(32, 'b'), upload(32, 'c'), key(64, 'd');
        auto fields = sender + route + "X-Chuan-Batch: " + batch + "\r\nX-Chuan-Offer-Async: 1\r\n";
        Require(Request(config.port, "POST", "/api/offer", fields, Hex("partial.bin") + "\tF\t1\t10\n").starts_with("HTTP/1.1 202"), "offer");
        fields = sender + route + "X-Chuan-Batch: " + batch + "\r\nX-Chuan-Item: 0\r\nX-Chuan-Upload: " + upload +
            "\r\nX-Chuan-Path: " + Hex("partial.bin") + "\r\nX-Chuan-Size: 10\r\nX-Chuan-Resume-Key: " + key + "\r\n";
        Require(Request(config.port, "POST", "/api/resume", fields).starts_with("HTTP/1.1 403"), "pending batch cannot resume");
        for (const auto &request : engine.GetSnapshot().transfers)
            if (request.requestOnly && request.state == State::Waiting) engine.DecideItems(request.id, "1");
        Require(engine.GetSnapshot().transfers.empty(), "remote browser consent is not a host receive placeholder");
        const auto waiting = engine.GetSnapshot(client);
        Require(waiting.transfers.size() == 1 && waiting.transfers.front().receiving &&
            waiting.transfers.front().activity == L"已接收，等待发送端开始", "recipient retains genuine wait before remote payload arrives");
        SingleWebJob(Request(config.port, "GET", "/web/api/state", localBrowserFields), waiting.transfers.front().id, State::Waiting);
        {
            Connection connection(config.port);
            connection.Send("POST /api/upload" + host + fields + "Expect: 100-continue\r\nContent-Length: 42\r\n\r\n");
            Require(connection.Header().starts_with("HTTP/1.1 100"), "approved payload");
            connection.Send("abc"); shutdown(connection.socket, SD_SEND);
            Until([&] { for (const auto &item : engine.GetSnapshot().transfers)
                if (item.name == L"partial.bin" && !item.requestOnly && item.state == State::Failed) return true; return false; });
        }
        const auto candidate = Request(config.port, "POST", "/api/resume", fields);
        Require(candidate.find("\"offset\":3") != std::string::npos, "resume uses relay prefix");
        auto wrongRoute = fields; wrongRoute.erase(wrongRoute.find(route), route.size());
        Require(Request(config.port, "POST", "/api/resume", wrongRoute + "X-Chuan-Resume-Reset: 1\r\n").starts_with("HTTP/1.1 403"), "resume route must match consent");
        Require(Request(config.port, "POST", "/api/resume", fields).find("\"offset\":3") != std::string::npos, "unauthorized reset preserves prefix");
        Require(Request(config.port, "POST", "/api/retry-upload/" + upload, fields).starts_with("HTTP/1.1 200"), "prepare retry");
        Connection resumed(config.port);
        resumed.Send("POST /api/upload" + host + fields +
            "X-Chuan-Offset: 3\r\nX-Chuan-Prefix: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\r\n"
            "Expect: 100-continue\r\nContent-Length: 39\r\n\r\n");
        Require(resumed.Header().starts_with("HTTP/1.1 100"), "accept resumed suffix");
        resumed.Send("defghij" + Unhex("72399361da6a7754fec986dca5b7cbaf1c810a28ded4abaf56b2106d06cb78b0"));
        uint64_t id = 0;
        Until([&] { for (const auto &item : engine.GetSnapshot().transfers)
            if (item.name == L"partial.bin" && !item.requestOnly && item.state == State::Waiting && !item.savedPath.empty()) { id = item.id; return true; } return false; });
        const auto downloaded = Request(config.port, "GET", "/web/files/" + std::to_string(id) + "?client=" + client);
        Require(downloaded.ends_with("\r\n\r\nabcdefghij"), "browser receives exact resumed contents");
        std::string response; do { response = resumed.Header(); } while (response.starts_with("HTTP/1.1 102"));
        Require(response.starts_with("HTTP/1.1 201"), "sender receives saved confirmation");
        std::cout << "PASS approved relay resumes at retained offset, preserves route ownership and delivers exact contents\n";
        Config gatewayConfig = config;
        gatewayConfig.id = L"receive-gateway"; gatewayConfig.dataDir = root / L"gateway-data";
        gatewayConfig.receiveDir = root / L"gateway-received";
        gatewayConfig.port = Port(SOCK_STREAM); gatewayConfig.discoveryPort = Port(SOCK_DGRAM);
        Engine gateway(gatewayConfig); Require(gateway.Start(error), "start gateway");
        Require(gateway.AddPeer("127.0.0.1:" + std::to_string(config.port), error), "connect receiver from gateway");
        Until([&] { for (const auto &peer : gateway.GetSnapshot().peers) if (peer.id == recipient) return true; return false; });
        const std::string gatewayClient(32, 'e'), forwardBatch(32, 'f');
        const auto gatewayState = Request(gatewayConfig.port, "GET", "/web/api/state", "X-Chuan-Client: " + gatewayClient + "\r\n");
        const auto gatewayTokenAt = gatewayState.find("\"token\":\""); Require(gatewayTokenAt != std::string::npos, "gateway session");
        const auto gatewayTokenStart = gatewayTokenAt + 9, gatewayTokenEnd = gatewayState.find('"', gatewayTokenStart);
        const auto browserFields = "X-Chuan-Client: " + gatewayClient + "\r\nX-Chuan-Token: " + gatewayState.substr(gatewayTokenStart, gatewayTokenEnd - gatewayTokenStart) +
            "\r\nX-Chuan-Sender: " + Hex("review") + "\r\nX-Chuan-Target: " + Hex(Utf8(recipient)) + "\r\nX-Chuan-Batch: " + forwardBatch + "\r\n";
        Require(Request(gatewayConfig.port, "POST", "/web/api/offer", browserFields + "X-Chuan-Offer-Async: 1\r\n", Hex("forwarded.zip") + "\tF\t1\t10\n").starts_with("HTTP/1.1 202"), "forwarded offer");
        Until([&] { for (const auto &item : engine.GetSnapshot().transfers)
            if (item.requestOnly && item.name == L"forwarded.zip" && item.state == State::Waiting) { engine.DecideItems(item.id, "1"); return true; } return false; });
        const auto probe = browserFields + "X-Chuan-Item: 0\r\nX-Chuan-Upload: " + upload + "\r\nX-Chuan-Path: " + Hex("forwarded.zip") +
            "\r\nX-Chuan-Size: 10\r\nX-Chuan-Resume-Key: " + key + "\r\nX-Chuan-Resume-Direct: 1\r\n";
        Until([&] { const auto answer = Request(gatewayConfig.port, "POST", "/web/api/resume", probe);
            return answer.starts_with("HTTP/1.1 200") && answer.find("\"offset\":0") != std::string::npos; });
        std::cout << "PASS gateway forwards the approved browser recipient in direct resume probes\n";
        gateway.Stop();
        engine.Stop(); return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
