// Included in core.cpp to share the bounded HTTP and socket helpers.
namespace {
std::wstring SystemHostName() {
    wchar_t name[256]{};
    DWORD length = 256;
    if (GetComputerNameW(name, &length) && length)
        return std::wstring(name, length);
    return L"Windows 电脑";
}
std::wstring ParseUserAgentDevice(const std::string &ua, const std::string &platform) {
    std::string lower = ua;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("ipad") != std::string::npos) return L"iPad";
    if (lower.find("iphone") != std::string::npos) return L"iPhone";
    if (lower.find("ipod") != std::string::npos) return L"iPod";
    if (const auto pos = lower.find("android"); pos != std::string::npos) {
        const auto semi = ua.find(';', pos);
        if (semi != std::string::npos) {
            auto end = ua.find_first_of(";)", semi + 1);
            std::string token = ua.substr(semi + 1, end == std::string::npos ? std::string::npos : end - semi - 1);
            if (const auto build = token.find(" Build/"); build != std::string::npos) token.resize(build);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.erase(token.begin());
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
            std::string lowToken = token;
            std::transform(lowToken.begin(), lowToken.end(), lowToken.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!token.empty() && token.size() <= 48 && token.find_first_of("\r\n|") == std::string::npos &&
                token != "K" && lowToken != "android" && lowToken != "mobile" && lowToken != "linux" && lowToken != "wv")
                return Wide(token);
        }
        return L"Android 手机";
    }
    if (lower.find("harmonyos") != std::string::npos || lower.find("openharmony") != std::string::npos) return L"HarmonyOS 设备";
    if (lower.find("windows") != std::string::npos) return L"Windows 电脑";
    if (lower.find("macintosh") != std::string::npos || lower.find("mac os x") != std::string::npos) return L"Mac";
    if (lower.find("cros") != std::string::npos) return L"Chromebook";
    if (lower.find("linux") != std::string::npos) return L"Linux 电脑";
    if (platform == "mobile-web") return L"移动设备";
    if (platform == "desktop-web") return L"电脑设备";
    return L"Web 设备";
}
std::wstring CleanHostName(std::string host, const std::string &ip) {
    while (!host.empty() && (host.back() == '.' || std::isspace(static_cast<unsigned char>(host.back())))) host.pop_back();
    auto lower = host;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string suffix : {".localdomain", ".local", ".lan", ".home"}) {
        if (lower.size() > suffix.size() && lower.ends_with(suffix)) {
            host.resize(host.size() - suffix.size());
            lower.resize(lower.size() - suffix.size());
            break;
        }
    }
    if (host.empty() || host == ip || host.size() > 48 || host.find_first_of("\r\n|") != std::string::npos) return {};
    in_addr parsed{};
    if (InetPtonA(AF_INET, host.c_str(), &parsed) == 1) return {};
    return Wide(host);
}
} // namespace
std::wstring Engine::HostNameLocked() const {
    return config_.name.empty() ? SystemHostName() : config_.name;
}
bool Engine::IsSelfIpLocked(const std::string &ip) const {
    if (ip.starts_with("127.")) return true;
    return std::any_of(networks_.begin(), networks_.end(), [&](const auto &n) { return n.text == ip; });
}
std::wstring Engine::BrowserNameLocked(const BrowserSession &session) {
    if (!session.name.empty()) return session.name;
    if (IsSelfIpLocked(session.ip)) {
        if (session.platform == "mobile-web" && !session.deviceModel.empty()) return session.deviceModel;
        return HostNameLocked();
    }
    if (const auto found = hostNames_.find(session.ip); found != hostNames_.end() && !found->second.first.empty())
        return found->second.first;
    if (!session.deviceModel.empty()) return session.deviceModel;
    return ParseUserAgentDevice(session.userAgent, session.platform);
}
void Engine::ResolveHostNameAsync(const std::string &ip) {
    {
        std::lock_guard lock(mutex_);
        if (IsSelfIpLocked(ip) || hostNames_.contains(ip) || hostNames_.size() >= 256) return;
        hostNames_[ip] = {L"", true};
    }
    Launch([this, ip] {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        std::wstring resolved;
        if (InetPtonA(AF_INET, ip.c_str(), &addr.sin_addr) == 1) {
            char host[NI_MAXHOST]{};
            if (getnameinfo(reinterpret_cast<const sockaddr *>(&addr), sizeof(addr), host, sizeof(host), nullptr, 0, NI_NAMEREQD) == 0)
                resolved = CleanHostName(host, ip);
        }
        std::lock_guard lock(mutex_);
        hostNames_[ip] = {resolved, false};
        if (!resolved.empty()) {
            for (const auto &[client, session] : browsers_) {
                if (session.ip == ip && session.name.empty()) {
                    for (auto &p : peers_) if (p.id == session.id) { p.name = resolved; refresh_ = true; }
                }
            }
        }
    });
}
void Engine::ExpireLocked() {
    const auto now = Now();
    for (auto it = browsers_.begin(); it != browsers_.end();) {
        auto &session = it->second;
        std::erase_if(session.tabs, [&](auto &tab) { return now - tab.second >= BrowserPresenceGrace; });
        if (session.tabs.empty())
            std::erase_if(peers_, [&](auto &p) { return p.id == session.id; });
        const bool idle = session.tabs.empty() && now - session.lastSeen > 60ull * 60 * 1000 &&
            std::none_of(jobs_.begin(), jobs_.end(), [&](const auto &j) { return j->webClient == it->first || j->recipient == it->first; }) &&
            std::none_of(webFiles_.begin(), webFiles_.end(), [&](const auto &w) { return w.second.owner == it->first || w.second.recipient == it->first; });
        if (idle) it = browsers_.erase(it);
        else ++it;
    }
    std::erase_if(peers_, [&](auto &p) { return !PeerSeenRecently(p,now); });
    const auto pause=[&](const auto &job,bool paused,const wchar_t *message){
        if(paused&&!job->browserPaused){job->resumeActivity=job->view.activity;job->view.activity=message;job->browserPaused=true;}
        else if(!paused&&job->browserPaused){job->view.activity=job->resumeActivity;job->resumeActivity.clear();job->browserPaused=false;}
    };
    for (auto &job : jobs_) if (job->announced && job->view.state==State::Queued) {
        if(!job->nativeOwner.empty()){
            uint64_t touched=job->view.started;for(const auto &[key,batch]:batchApprovals_)if(key.ends_with(":"+job->nativeOwner+":"+job->batchId))touched=std::max(touched,batch.touched);
            const bool active=std::any_of(jobs_.begin(),jobs_.end(),[&](const auto &item){return item!=job&&item->nativeOwner==job->nativeOwner&&item->batchId==job->batchId&&(item->paused||(!item->announced&&item->view.state<State::Completed));});
            if(now-touched>45000&&!active){job->announced=false;job->view.state=State::Failed;job->view.error=L"发送端已离线或停止，未收到剩余文件";job->finishedAt=now;job->cv.notify_all();}
            continue;
        }
        const auto sender=browsers_.find(job->webClient);
        if (sender==browsers_.end() || !sender->second.openTabs.contains(job->webTab)) {
            job->announced=false;job->view.state=State::Failed;job->view.error=L"发送页面已关闭，未收到此文件";job->finishedAt=now;job->cv.notify_all();
        }else{
            pause(job,sender->second.tabs.empty(),L"等待发送页面恢复");
        }
    }
    for (auto &job : jobs_) {
        if (!job->asyncOfferKey.empty() && !job->nativeOwner.empty() && job->view.state < State::Completed) {
            const auto batch=batchApprovals_.find(job->asyncOfferKey);
            const auto id=Wide(Unhex(job->nativeOwner));
            const bool live=std::any_of(peers_.begin(),peers_.end(),[&](const auto &peer){return peer.id==id;}) ||
                std::any_of(jobs_.begin(),jobs_.end(),[&](const auto &item){return !item->view.requestOnly&&item->nativeOwner==job->nativeOwner&&item->batchId==job->batchId&&(item->paused||item->view.state==State::Running);});
            if (!live && batch!=batchApprovals_.end() && now-batch->second.touched>45000) {
                job->cancel=true;CompleteOfferLocked(job,std::string(job->view.offerItems.size(),'0'));job->view.error=L"发送端已离线，接收请求已取消";
            }
        }
        if (!job->asyncOfferKey.empty() && !job->webClient.empty() && job->view.state < State::Completed) {
            const auto sender = browsers_.find(job->webClient);
            if (sender == browsers_.end() || !sender->second.openTabs.contains(job->webTab)) {
                job->cancel = true; job->view.awaitingApproval = false; job->view.state = State::Cancelled;
                job->view.error = L"发送页面已关闭，接收请求已取消"; job->finishedAt = now; job->cv.notify_all();
                std::lock_guard socketsLock(socketsMutex_);
                for (const auto &[socket, owner] : sockets_) if (owner.lock() == job) shutdown(socket, SD_BOTH);
            }
        }
        if (job->direct && job->view.state < State::Completed) {
            const bool senderLive = std::any_of(peers_.begin(), peers_.end(), [&](const auto &peer) {
                return !peer.browser && peer.id == job->directSenderId;
            });
            if (senderLive) job->directOfflineAt = 0;
            else if (!job->directOfflineAt) job->directOfflineAt = now;
            else if (now - job->directOfflineAt >= 8000) {
                job->cancel = true; job->paused = false; job->view.paused = false;
                job->view.state = State::Cancelled; job->view.speed = 0;
                job->view.error = L"发送端已离线，传输已取消"; job->finishedAt = now; job->cv.notify_all();
                continue;
            }
        }
        if (job->directOutgoing && job->view.state == State::Waiting && !job->directGatewayId.empty()) {
            const bool gwLive = std::any_of(peers_.begin(), peers_.end(), [&](const auto &peer) {
                return !peer.browser && peer.id == job->directGatewayId;
            });
            if (gwLive) job->directOfflineAt = 0;
            else if (!job->directOfflineAt) job->directOfflineAt = now;
            else if (now - job->directOfflineAt >= 8000) {
                job->cancel = true; job->paused = false; job->view.paused = false;
                job->view.state = State::Cancelled; job->view.speed = 0;
                job->view.error = L"Web 服务主机已离线，传输已取消"; job->finishedAt = now; job->cv.notify_all();
                directFiles_.erase(job->view.id);
                continue;
            }
        }
        if (job->recipient.empty() || (job->view.state != State::Waiting && !(job->direct && job->view.state == State::Running))) continue;
        auto session = browsers_.find(job->recipient);
        if (session != browsers_.end() && !session->second.openTabs.empty()) {
            if (job->view.state == State::Waiting) pause(job,session->second.tabs.empty(),L"等待接收页面恢复");
            continue;
        }
        job->cancel = true;
        job->view.awaitingApproval = false;
        job->view.state = State::Cancelled;
        job->view.error = L"接收页面已关闭";
        job->finishedAt = now;
        job->cv.notify_all();
        if (job->direct) SendDirectControl(job, "cancel");
    }
    // A running native host itself keeps this LAN session alive. The ledger is memory-only.
}
void Engine::SetNote(const std::string &ip, const std::wstring &note) {
    if (note.size() > 48 || note.find_first_of(L"\r\n|") != std::wstring::npos) return;
    std::lock_guard lock(mutex_); notes_[ip] = note;
}
std::wstring Engine::GetNote(const std::string &ip) {
    std::lock_guard lock(mutex_);
    auto it = notes_.find(ip); return it == notes_.end() ? L"" : it->second;
}
bool Engine::RequestPeer(const Peer &peer, const std::string &path, const std::string &extra, std::string &body) {
    const auto address = peer.gateway.empty() ? peer.ip : peer.gateway;
    SOCKET s = Connect(address, peer.port);
    if (s == INVALID_SOCKET) return false;
    TrackSocket(s); SetTimeout(s, 4500, 2000);
    Config config; std::wstring hostName;
    { std::lock_guard lock(mutex_); config = config_; hostName = HostNameLocked(); }
    const bool mesh = path == "/api/mesh";
    const auto method = mesh || path == "/api/ping" || path == "/api/leave" ? "POST " : "GET ";
    std::string request = std::string(method) + path + " HTTP/1.1\r\nHost: " + address + ":" +
        std::to_string(peer.port) + "\r\nConnection: close\r\nContent-Length: " + std::to_string(mesh ? extra.size() : 0) + "\r\nX-Chuan-Id: " +
        Hex(Utf8(config.id)) + "\r\nX-Chuan-Sender: " + Hex(Utf8(hostName)) +
        "\r\nX-Chuan-Server: " + Utf8(peer.owner.empty() ? peer.id : peer.owner) +
        "\r\nX-Chuan-Platform: windows\r\nX-Chuan-Port: " + std::to_string(boundPort_) + "\r\n" + (mesh ? "\r\n" + extra : extra + "\r\n");
    std::string first; std::map<std::string, std::string> headers; uint64_t length = 0;
    bool ok = SendAll(s, request) && ReadHeader(s, first, headers) && first.starts_with("HTTP/1.1 200 ") &&
        Number(Get(headers, "content-length"), length) && length <= 1024 * 1024;
    if (ok) { body.resize(static_cast<size_t>(length)); ok = ReadExact(s, body.data(), body.size()); }
    ReleaseSocket(s); return ok;
}
std::string Engine::Mesh(const std::string &) {
    std::lock_guard lock(mutex_); ExpireLocked();
    std::wstring leader = config_.id;
    for (auto &p : peers_) if (!p.browser && p.id < leader) leader = p.id;
    std::ostringstream out;
    out << "L " << Hex(Utf8(leader)) << '\n';
    for (auto &[keyValue, session] : browsers_) {
        if (session.tabs.empty()) continue;
        const auto name = BrowserNameLocked(session);
        out << "B " << Hex(Utf8(session.id)) << ' ' << Hex(Utf8(name)) << ' ' << session.ip << ' ' << (session.platform.empty()?"web":session.platform) << ' ' << (session.directoryReceive?"D":"-") << '\n';
    }
    return out.str();
}
void Engine::SyncMesh(const Peer &peer) {
    std::string body;
    if (!RequestPeer(peer, "/api/mesh", "", body)) return;
    std::lock_guard lock(mutex_);
    std::set<std::wstring> browserIds;
    std::istringstream lines(body); std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line); std::string type, id, name, ip;
        fields >> type >> id;
        if (type == "B" && fields >> name >> ip && IsLocalLocked(ip)) {
            Peer browser{Wide(Unhex(id)), Wide(Unhex(name)), ip, peer.port, Now(), false, true};
            std::string platform;fields>>platform;browser.platform=platform=="mobile-web"||platform=="desktop-web"?platform:"web";std::string capability;fields>>capability;browser.directoryReceive=capability=="D";
            if (!browser.id.starts_with(L"web:") || browser.name.size() > 64) continue;
            if (browser.name.empty()) browser.name = browser.platform == "mobile-web" ? L"移动设备" : browser.platform == "desktop-web" ? L"电脑设备" : L"Web 设备";
            browser.gateway = peer.ip; browser.owner = peer.id;
            browserIds.insert(browser.id);
            auto found = std::find_if(peers_.begin(), peers_.end(), [&](auto &p) { return p.id == browser.id; });
            if (found == peers_.end()) peers_.push_back(browser);
            else if (found->owner == peer.id) *found = browser;
        }
    }
    std::erase_if(peers_, [&](auto &p) { return p.owner == peer.id && !browserIds.contains(p.id); });
}
bool Engine::PingNow(const Peer &peer, std::wstring &error) {
    if (peer.browser && peer.owner.empty()) {
        std::string key; uint64_t sequence = 0;
        { std::lock_guard lock(mutex_);
          for (auto &[client, session] : browsers_) if (session.id == peer.id && !session.tabs.empty()) {
              key = client; sequence = ++session.ping; session.pingAt = Now(); break;
          } }
        if (key.empty()) { error = L"目标设备已离线"; return false; }
        auto deadline = Now() + 3500;
        while (running_ && Now() < deadline) {
            { std::lock_guard lock(mutex_); if (browsers_.at(key).ack >= sequence) return true; }
            Sleep(30);
        }
        error = L"页面未确认 Ping，可能已暂停或离线"; return false;
    }
    std::string body;
    bool ok = RequestPeer(peer, "/api/ping", "X-Chuan-Target: " + Hex(Utf8(peer.id)) + "\r\n", body);
    if (!ok) error = L"Ping 超时或目标设备不可达";
    return ok;
}
void Engine::Ping(const Peer &peer) {
    { std::lock_guard lock(mutex_); auto &at = pingTimes_["desktop"];
      if (Now() - at < 800) return; at = Now(); }
    if (!Launch([this, peer] { std::wstring error; Notice(PingNow(peer, error) ? L"Ping 已送达" : error); }))
        Notice(L"当前任务较多，请稍后重试");
}
void Engine::RemoveRelayFile(const std::shared_ptr<Job> &job) {
    if (!job || !job->staging || job->direct || job->view.savedPath.empty()) return;
    fs::path path, relayRoot;
    {
        std::lock_guard lock(mutex_);
        relayRoot = config_.dataDir / L"relay";
        path = job->view.savedPath;
        if (path.empty() || !SafeWebPath(path, relayRoot)) return;
        const bool busy = std::any_of(jobs_.begin(), jobs_.end(), [&](const auto &other) {
            return other != job && other->view.savedPath == path && (other->paused || other->view.state < State::Completed);
        });
        if (busy) return;
        std::erase_if(webFiles_, [&](const auto &entry) { return entry.second.path == path; });
        job->view.savedPath.clear();
        job->hasIdentity = false;
    }
    DeleteFileW(Win32Path(path).c_str());
}
void Engine::CleanRelay() {
    std::vector<uint64_t> expired;
    Config cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = config_;
        for (auto &j : jobs_) if (j->staging && (!running_ ||
            (j->view.state >= State::Completed && j->finishedAt && Now() - j->finishedAt > 600000))) {
            if (!running_) j->view.state = State::Cancelled;
            expired.push_back(j->view.id);
        }
    }
    for (auto id : expired) { std::wstring error; DeleteTransfer(id, true, error); }
    const auto now = Now();
    std::vector<fs::path> staleParts;
    std::set<fs::path> activeParts;
    {
        std::lock_guard lock(mutex_);
        if (jobs_.size() > 500) {
            size_t excess = jobs_.size() - 500;
            std::erase_if(jobs_, [&](const auto &j) {
                if (!excess) return false;
                const bool drop = (j->hidden || j->view.removed) && j->view.state >= State::Completed &&
                    !j->paused && !j->retryDeadline && j->asyncOfferKey.empty() && j->finishedAt &&
                    now - j->finishedAt > 30ull * 60 * 1000 &&
                    std::none_of(webFiles_.begin(), webFiles_.end(), [&](const auto &w) { return w.second.delivery == j; });
                if (drop) --excess;
                return drop;
            });
        }
        for (const auto &j : jobs_) if (!j->partialPath.empty() && (j->paused || j->view.state < State::Completed || j->retryDeadline)) activeParts.insert(j->partialPath);
    }
    auto scanStaleParts = [&](const fs::path &dir) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return;
        FILETIME ftNow{}; GetSystemTimeAsFileTime(&ftNow);
        const ULARGE_INTEGER now100ns{{ftNow.dwLowDateTime, ftNow.dwHighDateTime}};
        for (const auto &entry : fs::directory_iterator(dir, ec)) {
            if (ec || !entry.is_regular_file(ec)) continue;
            const auto fname = entry.path().filename().wstring();
            if (!fname.starts_with(L".chuan-") || !fname.ends_with(L".part")) continue;
            if (activeParts.contains(entry.path())) continue;
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &data)) continue;
            const ULARGE_INTEGER mtime{{data.ftLastWriteTime.dwLowDateTime, data.ftLastWriteTime.dwHighDateTime}};
            if (now100ns.QuadPart > mtime.QuadPart && (now100ns.QuadPart - mtime.QuadPart) > 7ull * 24 * 60 * 60 * 10000000ull)
                staleParts.push_back(entry.path());
        }
    };
    scanStaleParts(cfg.receiveDir);
    scanStaleParts(cfg.dataDir / L"relay");
    for (const auto &p : staleParts) { std::error_code ec; fs::remove(p, ec); }
}
